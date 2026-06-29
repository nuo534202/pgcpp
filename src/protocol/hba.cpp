// hba.cpp — pg_hba.conf parser and lookup.
//
// The parser is line-oriented. Each non-empty, non-comment line is split into
// whitespace-separated tokens. The first token is the connection type, then
// database, user, (address for host*), method, and optional method options.
//
// Quoted strings (with backslash escapes) are supported per PG's lexer.
// Continuation lines (backslash at end of line) are not supported (pgcpp
// uses a simplified parser).
#include "protocol/hba.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>

#include "protocol/ifaddr.hpp"

namespace pgcpp::protocol {

namespace {

// Tokenize a line into whitespace-separated tokens, honouring double-quoted
// strings (PG allows quotes around tokens containing special characters).
std::vector<std::string> Tokenize(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool in_quotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (in_quotes) {
            if (c == '\\' && i + 1 < line.size()) {
                cur.push_back(line[i + 1]);
                ++i;
            } else if (c == '"') {
                in_quotes = false;
            } else {
                cur.push_back(c);
            }
            continue;
        }
        if (c == '"') {
            in_quotes = true;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
            continue;
        }
        if (c == '#') {
            // Comment — ignore rest of line.
            break;
        }
        cur.push_back(c);
    }
    if (!cur.empty()) {
        out.push_back(cur);
    }
    return out;
}

HbaConnType ParseConnType(const std::string& s, bool& ok) {
    ok = true;
    if (s == "local")
        return HbaConnType::kLocal;
    if (s == "host")
        return HbaConnType::kHost;
    if (s == "hostssl")
        return HbaConnType::kHostSsl;
    if (s == "hostnossl")
        return HbaConnType::kHostNoSsl;
    ok = false;
    return HbaConnType::kLocal;
}

// Split "addr/mask" into the address part and mask bits.
// If no slash, mask_bits is set to 32 (IPv4) or 0 (no match for IPv6).
void SplitCidr(const std::string& tok, std::string& addr, int& mask_bits) {
    size_t slash = tok.find('/');
    if (slash == std::string::npos) {
        addr = tok;
        mask_bits = 32;
        return;
    }
    addr = tok.substr(0, slash);
    try {
        mask_bits = std::stoi(tok.substr(slash + 1));
    } catch (...) {
        mask_bits = 32;
    }
}

}  // namespace

HbaMethod ParseHbaMethod(const std::string& name) {
    if (name == "trust")
        return HbaMethod::kTrust;
    if (name == "reject")
        return HbaMethod::kReject;
    if (name == "password")
        return HbaMethod::kPassword;
    if (name == "md5")
        return HbaMethod::kMd5;
    if (name == "scram-sha-256")
        return HbaMethod::kScramSha256;
    if (name == "gss")
        return HbaMethod::kGss;
    if (name == "peer")
        return HbaMethod::kPeer;
    if (name == "ident")
        return HbaMethod::kIdent;
    if (name == "cert")
        return HbaMethod::kCert;
    if (name == "radius")
        return HbaMethod::kRadius;
    if (name == "ldap")
        return HbaMethod::kLdap;
    if (name == "pam")
        return HbaMethod::kPam;
    return HbaMethod::kUnsupported;
}

std::string HbaMethodToString(HbaMethod method) {
    switch (method) {
        case HbaMethod::kTrust:
            return "trust";
        case HbaMethod::kReject:
            return "reject";
        case HbaMethod::kPassword:
            return "password";
        case HbaMethod::kMd5:
            return "md5";
        case HbaMethod::kScramSha256:
            return "scram-sha-256";
        case HbaMethod::kGss:
            return "gss";
        case HbaMethod::kPeer:
            return "peer";
        case HbaMethod::kIdent:
            return "ident";
        case HbaMethod::kCert:
            return "cert";
        case HbaMethod::kRadius:
            return "radius";
        case HbaMethod::kLdap:
            return "ldap";
        case HbaMethod::kPam:
            return "pam";
        case HbaMethod::kUnsupported:
            return "unsupported";
    }
    return "unknown";
}

bool MatchDatabaseOrUser(const std::string& token_list, const std::string& name) {
    // Split by comma.
    std::string cur;
    auto check_one = [&](const std::string& tok) {
        if (tok == "all")
            return true;
        if (tok == "sameuser")
            return name == "sameuser";
        if (tok == "replication")
            return name == "replication";
        // "+" prefix means "members of role" (not supported in pgcpp; treat as
        // a literal role name without the +).
        std::string t = tok;
        if (!t.empty() && t[0] == '+')
            t = t.substr(1);
        return t == name;
    };
    for (char c : token_list) {
        if (c == ',') {
            if (!cur.empty() && check_one(cur))
                return true;
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty() && check_one(cur))
        return true;
    return false;
}

bool MatchCidr(const std::string& addr, const std::string& cidr, int mask_bits) {
    uint32_t a, c;
    if (!ParseIPv4(addr, a))
        return false;
    if (!ParseIPv4(cidr, c))
        return false;
    if (mask_bits == 0)
        return true;
    if (mask_bits > 32)
        return false;
    // Convert mask bits to a uint32_t mask (high bits set).
    uint32_t mask = mask_bits == 32 ? 0xffffffffu : (0xffffffffu << (32 - mask_bits));
    return (a & mask) == (c & mask);
}

HbaConfig ParseHbaConfig(const std::string& text) {
    HbaConfig cfg;
    std::istringstream ss(text);
    std::string line;
    int lineno = 0;
    while (std::getline(ss, line)) {
        ++lineno;
        auto tokens = Tokenize(line);
        if (tokens.empty())
            continue;  // blank or comment
        HbaLine hba;
        hba.lineno = lineno;
        hba.raw = line;
        bool ok;
        hba.conn_type = ParseConnType(tokens[0], ok);
        if (!ok) {
            cfg.valid = false;
            cfg.error = "unknown connection type: " + tokens[0];
            cfg.error_lineno = lineno;
            return cfg;
        }
        size_t idx = 1;
        if (idx >= tokens.size()) {
            cfg.valid = false;
            cfg.error = "missing database field";
            cfg.error_lineno = lineno;
            return cfg;
        }
        hba.databases = tokens[idx++];
        if (idx >= tokens.size()) {
            cfg.valid = false;
            cfg.error = "missing user field";
            cfg.error_lineno = lineno;
            return cfg;
        }
        hba.users = tokens[idx++];
        // For host/hostssl/hostnossl: next token is the address (CIDR or
        // "samehost"/"samenet"/"all"). For "local", there is no address.
        if (hba.conn_type != HbaConnType::kLocal) {
            if (idx >= tokens.size()) {
                cfg.valid = false;
                cfg.error = "missing address field";
                cfg.error_lineno = lineno;
                return cfg;
            }
            std::string addr_tok = tokens[idx++];
            if (addr_tok == "all" || addr_tok == "samehost" || addr_tok == "samenet") {
                hba.cidr = addr_tok;  // sentinel; matched at lookup time
                hba.mask_bits = -1;   // marker
            } else {
                SplitCidr(addr_tok, hba.cidr, hba.mask_bits);
            }
        }
        if (idx >= tokens.size()) {
            cfg.valid = false;
            cfg.error = "missing method field";
            cfg.error_lineno = lineno;
            return cfg;
        }
        hba.method = ParseHbaMethod(tokens[idx++]);
        if (hba.method == HbaMethod::kUnsupported) {
            cfg.valid = false;
            cfg.error = "unknown auth method: " + tokens[idx - 1];
            cfg.error_lineno = lineno;
            return cfg;
        }
        // Remaining tokens are method options (ignored by pgcpp).
        cfg.lines.push_back(std::move(hba));
    }
    return cfg;
}

const HbaLine* SelectHbaLine(const HbaConfig& config, const std::string& database,
                             const std::string& user, const std::string& addr, bool ssl_in_use) {
    for (const auto& line : config.lines) {
        // Connection-type filter.
        if (line.conn_type == HbaConnType::kHostSsl && !ssl_in_use)
            continue;
        if (line.conn_type == HbaConnType::kHostNoSsl && ssl_in_use)
            continue;
        if (line.conn_type == HbaConnType::kLocal && !addr.empty())
            continue;
        // Database / user filter.
        if (!MatchDatabaseOrUser(line.databases, database))
            continue;
        if (!MatchDatabaseOrUser(line.users, user))
            continue;
        // Address filter (only for host*).
        if (line.conn_type != HbaConnType::kLocal) {
            if (line.mask_bits == -1) {
                // "all" / "samehost" / "samenet"
                if (line.cidr == "all") {
                    // matches any address
                } else if (line.cidr == "samehost") {
                    if (!IsSameHost(addr))
                        continue;
                } else if (line.cidr == "samenet") {
                    if (!IsSameNet(addr))
                        continue;
                }
            } else {
                if (!MatchCidr(addr, line.cidr, line.mask_bits))
                    continue;
            }
        }
        return &line;
    }
    return nullptr;
}

}  // namespace pgcpp::protocol

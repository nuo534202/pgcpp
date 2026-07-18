// conninfo.cpp — Connection string / URI parser implementation (P3-11).
//
// Mirrors libpq's conninfo_parse + conninfo_uri_parse from fe-connect.c.
// Supports:
//   1. Keyword=value lists (whitespace separated, values may be quoted):
//        host=localhost port=5432 user=alice dbname=testdb
//   2. URI form (postgresql:// or postgres:// scheme):
//        postgresql://alice:secret@localhost:5432/testdb?sslmode=disable
//
// Quoted values use single quotes and accept backslash escapes:
//   password='it\'s a secret'
//   options='-c geqo=off'
#include "libpq/conninfo.hpp"

#include <arpa/inet.h>
#include <pwd.h>
#include <unistd.h>

#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace pgcpp::libpq {

namespace {

// IsKeywordStartChar — first char of a keyword must be alphabetic or
// underscore (matches libpq's conninfo_scan).
bool IsKeywordStartChar(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// IsKeywordChar — subsequent chars of a keyword.
bool IsKeywordChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// SkipWhitespace — advance pos past spaces/tabs.
std::size_t SkipWhitespace(const std::string& s, std::size_t pos) {
    while (pos < s.size() &&
           (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r')) {
        ++pos;
    }
    return pos;
}

// ParseKeyword — read a keyword starting at pos. Returns the keyword
// string (or empty if no keyword char at pos) and advances pos.
std::string ParseKeyword(const std::string& s, std::size_t& pos) {
    std::string kw;
    if (pos >= s.size() || !IsKeywordStartChar(s[pos])) {
        return kw;
    }
    while (pos < s.size() && IsKeywordChar(s[pos])) {
        kw.push_back(s[pos]);
        ++pos;
    }
    return kw;
}

// ParseQuotedValue — read a single-quoted value starting at pos (which
// points at the opening quote). Handles backslash escapes. Returns the
// decoded value and advances pos to the char after the closing quote.
bool ParseQuotedValue(const std::string& s, std::size_t& pos, std::string& out) {
    if (pos >= s.size() || s[pos] != '\'') {
        return false;
    }
    ++pos;  // skip opening quote
    out.clear();
    while (pos < s.size()) {
        char c = s[pos];
        if (c == '\\' && pos + 1 < s.size()) {
            // Backslash escape: keep the next char literally.
            out.push_back(s[pos + 1]);
            pos += 2;
        } else if (c == '\'') {
            if (pos + 1 < s.size() && s[pos + 1] == '\'') {
                // SQL-standard doubled single quote → one quote.
                out.push_back('\'');
                pos += 2;
            } else {
                ++pos;  // skip closing quote
                return true;
            }
        } else {
            out.push_back(c);
            ++pos;
        }
    }
    // Ran off end without a closing quote.
    return false;
}

// ParseUnquotedValue — read a value that is not quoted. Stops at
// whitespace. Backslash escapes are honored (e.g. "\ " = literal space).
std::string ParseUnquotedValue(const std::string& s, std::size_t& pos) {
    std::string val;
    while (pos < s.size()) {
        char c = s[pos];
        if (c == '\\' && pos + 1 < s.size()) {
            val.push_back(s[pos + 1]);
            pos += 2;
        } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            break;
        } else {
            val.push_back(c);
            ++pos;
        }
    }
    return val;
}

// ParseKeywordValueList — parse "keyword=value keyword=value" form.
bool ParseKeywordValueList(const std::string& s, std::vector<ConnInfoOption>& out,
                           std::string& errmsg) {
    std::size_t pos = 0;
    while (pos < s.size()) {
        pos = SkipWhitespace(s, pos);
        if (pos >= s.size()) {
            break;
        }
        // End-of-string or terminator.
        if (s[pos] == '\0') {
            break;
        }
        std::string kw = ParseKeyword(s, pos);
        if (kw.empty()) {
            errmsg = "missing keyword at position " + std::to_string(pos);
            return false;
        }
        pos = SkipWhitespace(s, pos);
        if (pos >= s.size() || s[pos] != '=') {
            errmsg = "missing '=' after keyword '" + kw + "'";
            return false;
        }
        ++pos;  // skip '='
        pos = SkipWhitespace(s, pos);

        std::string val;
        if (pos < s.size() && s[pos] == '\'') {
            if (!ParseQuotedValue(s, pos, val)) {
                errmsg = "unterminated quoted string for keyword '" + kw + "'";
                return false;
            }
        } else {
            val = ParseUnquotedValue(s, pos);
        }
        // Append (or update existing keyword).
        bool found = false;
        for (auto& opt : out) {
            if (opt.keyword == kw) {
                opt.val = val;
                found = true;
                break;
            }
        }
        if (!found) {
            out.push_back({kw, val, "", "", 0});
        }
    }
    return true;
}

// ParseUri — parse a postgresql:// or postgres:// URI.
//
// URI form (RFC 3986 subset, matches libpq):
//   postgresql://[user[:password]@][host][:port][/dbname][?param=value&...]
bool ParseUri(const std::string& s, std::vector<ConnInfoOption>& out, std::string& errmsg) {
    std::size_t pos = 0;
    // Scheme.
    if (s.compare(0, 13, "postgresql://") == 0) {
        pos = 13;
    } else if (s.compare(0, 11, "postgres://") == 0) {
        pos = 11;
    } else {
        errmsg = "URI does not start with postgresql:// or postgres://";
        return false;
    }

    // Optional userinfo (user[:password]@) — ends at first '/' or ':' or
    // '@' that closes the userinfo.
    std::size_t at_pos = s.find('@', pos);
    std::size_t slash_pos = s.find('/', pos);
    std::size_t qmark_pos = s.find('?', pos);

    // Find the boundaries: userinfo ends at '@', host ends at ':' (port)
    // or '/' (path) or '?' (query) or end of string.
    if (at_pos != std::string::npos && (slash_pos == std::string::npos || at_pos < slash_pos) &&
        (qmark_pos == std::string::npos || at_pos < qmark_pos)) {
        // Userinfo is present.
        std::string userinfo = s.substr(pos, at_pos - pos);
        std::size_t colon_in_userinfo = userinfo.find(':');
        if (colon_in_userinfo != std::string::npos) {
            std::string user = userinfo.substr(0, colon_in_userinfo);
            std::string pass = userinfo.substr(colon_in_userinfo + 1);
            SetOption(out, "user", user);
            SetOption(out, "password", pass);
        } else {
            SetOption(out, "user", userinfo);
        }
        pos = at_pos + 1;
    }

    // Determine where host ends: next of ':', '/', '?', or end of string.
    std::size_t next_colon = s.find(':', pos);
    std::size_t next_slash = s.find('/', pos);
    std::size_t next_qmark = s.find('?', pos);

    // Host may be enclosed in [] for IPv6.
    if (pos < s.size() && s[pos] == '[') {
        std::size_t bracket_end = s.find(']', pos);
        if (bracket_end == std::string::npos) {
            errmsg = "unterminated IPv6 host in URI";
            return false;
        }
        std::string host = s.substr(pos + 1, bracket_end - pos - 1);
        SetOption(out, "host", host);
        pos = bracket_end + 1;
    } else {
        // Find earliest of : / ? / end.
        std::size_t earliest = s.size();
        if (next_colon != std::string::npos && next_colon < earliest) {
            earliest = next_colon;
        }
        if (next_slash != std::string::npos && next_slash < earliest) {
            earliest = next_slash;
        }
        if (next_qmark != std::string::npos && next_qmark < earliest) {
            earliest = next_qmark;
        }
        std::string host = s.substr(pos, earliest - pos);
        if (!host.empty()) {
            SetOption(out, "host", host);
        }
        pos = earliest;
    }

    // Port.
    if (pos < s.size() && s[pos] == ':') {
        ++pos;
        std::size_t port_end = s.size();
        if (next_slash != std::string::npos && next_slash > pos) {
            port_end = next_slash;
        }
        if (next_qmark != std::string::npos && next_qmark > pos && next_qmark < port_end) {
            port_end = next_qmark;
        }
        std::string port_str = s.substr(pos, port_end - pos);
        SetOption(out, "port", port_str);
        pos = port_end;
    }

    // Path → dbname.
    if (pos < s.size() && s[pos] == '/') {
        ++pos;
        std::size_t db_end = s.size();
        if (next_qmark != std::string::npos && next_qmark > pos) {
            db_end = next_qmark;
        }
        std::string dbname = s.substr(pos, db_end - pos);
        if (!dbname.empty()) {
            SetOption(out, "dbname", dbname);
        }
        pos = db_end;
    }

    // Query parameters.
    if (pos < s.size() && s[pos] == '?') {
        ++pos;
        while (pos < s.size()) {
            std::size_t amp_pos = s.find('&', pos);
            std::string param =
                (amp_pos == std::string::npos) ? s.substr(pos) : s.substr(pos, amp_pos - pos);
            std::size_t eq_pos = param.find('=');
            if (eq_pos != std::string::npos) {
                std::string key = param.substr(0, eq_pos);
                std::string val = param.substr(eq_pos + 1);
                SetOption(out, key, val);
            }
            if (amp_pos == std::string::npos) {
                break;
            }
            pos = amp_pos + 1;
        }
    }

    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool ParseConnInfo(const std::string& conninfo, std::vector<ConnInfoOption>& out,
                   std::string& errmsg) {
    if (conninfo.empty()) {
        return true;  // empty input → empty output (FillDefaults fills later)
    }
    // Detect URI form.
    if (conninfo.compare(0, 13, "postgresql://") == 0 ||
        conninfo.compare(0, 11, "postgres://") == 0) {
        return ParseUri(conninfo, out, errmsg);
    }
    return ParseKeywordValueList(conninfo, out, errmsg);
}

void FillDefaults(std::vector<ConnInfoOption>& opts) {
    static const struct {
        const char* keyword;
        const char* label;
        const char* dispchar;
    } kDefaultKeys[] = {
        {"host", "Database host", ""},
        {"hostaddr", "Host address (IP)", ""},
        {"port", "Database port", ""},
        {"user", "Database user", ""},
        {"password", "Database password", "*"},
        {"dbname", "Database name", ""},
        {"options", "Backend options", ""},
        {"connect_timeout", "Connect timeout (s)", ""},
        {"sslmode", "SSL mode", ""},
        {"requiressl", "Require SSL (deprecated)", ""},
        {"service", "Service name", ""},
        {"target_session_attrs", "Target session attributes", ""},
    };

    for (const auto& k : kDefaultKeys) {
        bool found = false;
        for (const auto& opt : opts) {
            if (opt.keyword == k.keyword) {
                found = true;
                break;
            }
        }
        if (!found) {
            opts.push_back({k.keyword, "", k.label, k.dispchar, 0});
        }
    }
    // Apply sensible defaults.
    if (const std::string* v = GetOption(opts, "host"); v != nullptr && v->empty()) {
        SetOption(opts, "host", DefaultHost());
    }
    if (const std::string* v = GetOption(opts, "port"); v != nullptr && v->empty()) {
        SetOption(opts, "port", std::to_string(DefaultPort()));
    }
    if (const std::string* v = GetOption(opts, "user"); v != nullptr && v->empty()) {
        SetOption(opts, "user", DefaultUser());
    }
}

const std::string* GetOption(const std::vector<ConnInfoOption>& opts, const std::string& keyword) {
    for (const auto& opt : opts) {
        if (opt.keyword == keyword) {
            return &opt.val;
        }
    }
    return nullptr;
}

void SetOption(std::vector<ConnInfoOption>& opts, const std::string& keyword,
               const std::string& val) {
    for (auto& opt : opts) {
        if (opt.keyword == keyword) {
            opt.val = val;
            return;
        }
    }
    opts.push_back({keyword, val, "", "", 0});
}

std::string BuildConninfoString(const std::vector<ConnInfoOption>& opts) {
    std::string out;
    for (std::size_t i = 0; i < opts.size(); ++i) {
        if (opts[i].val.empty()) {
            continue;
        }
        if (!out.empty()) {
            out.push_back(' ');
        }
        out += opts[i].keyword;
        out.push_back('=');
        // Quote the value if it contains whitespace, single quote, or
        // backslash.
        const std::string& v = opts[i].val;
        bool need_quote = false;
        for (char c : v) {
            if (c == ' ' || c == '\t' || c == '\'' || c == '\\') {
                need_quote = true;
                break;
            }
        }
        if (!need_quote) {
            out += v;
        } else {
            out.push_back('\'');
            for (char c : v) {
                if (c == '\'' || c == '\\') {
                    out.push_back('\\');
                }
                out.push_back(c);
            }
            out.push_back('\'');
        }
    }
    return out;
}

std::string DefaultUser() {
    const char* env = std::getenv("PGUSER");
    if (env != nullptr && env[0] != '\0') {
        return env;
    }
    struct passwd* pw = getpwuid(getuid());
    if (pw != nullptr && pw->pw_name != nullptr) {
        return pw->pw_name;
    }
    return "postgres";
}

std::string DefaultHost() {
    const char* env = std::getenv("PGHOST");
    if (env != nullptr && env[0] != '\0') {
        return env;
    }
    return "localhost";
}

int DefaultPort() {
    const char* env = std::getenv("PGPORT");
    if (env != nullptr && env[0] != '\0') {
        int port = std::atoi(env);
        if (port > 0) {
            return port;
        }
    }
    return 5432;
}

}  // namespace pgcpp::libpq

// hba.h — pg_hba.conf client authentication configuration file parser.
//
// Converted from PostgreSQL 15's src/backend/libpq/hba.c.
//
// pg_hba.conf is a flat text file. Each non-empty, non-comment line specifies
// a connection rule: connection type, database, user, client address, and
// authentication method. The first matching rule wins.
//
// Line format (PG 15):
//   local    DATABASE  USER  METHOD  [OPTIONS]
//   host     DATABASE  USER  ADDRESS  METHOD  [OPTIONS]
//   hostssl  DATABASE  USER  ADDRESS  METHOD  [OPTIONS]
//   hostnossl DATABASE USER  ADDRESS  METHOD  [OPTIONS]
//
// DATABASE/USER support comma-separated lists and the "all" wildcard.
// ADDRESS is a CIDR mask (e.g. 192.168.1.0/24) or "sameuser"/"replication".
// METHOD is one of: trust, reject, password, md5, scram-sha-256, gss, sspi,
// peer, ident, radius, cert, pam, ldap.
//
// pgcpp provides a faithful line-by-line parser plus a lookup function
// (SelectHbaLine) that picks the first line matching a connection.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pgcpp::protocol {

// HbaConnType — connection type from the first column of pg_hba.conf.
enum class HbaConnType {
    kLocal,      // Unix-domain socket
    kHost,       // TCP (any)
    kHostSsl,    // TCP requiring SSL
    kHostNoSsl,  // TCP requiring no SSL
};

// HbaMethod — authentication method (subset of PG's UserAuth enum).
enum class HbaMethod {
    kTrust,
    kReject,
    kPassword,
    kMd5,
    kScramSha256,
    kGss,
    kPeer,
    kIdent,
    kCert,
    kRadius,
    kLdap,
    kPam,
    kUnsupported,
};

// HbaLine — one parsed rule from pg_hba.conf.
struct HbaLine {
    HbaConnType conn_type = HbaConnType::kLocal;
    // Raw token ranges for database/user (each may be a comma-separated list,
    // or "all"/"sameuser"/"replication"). Stored as raw text for matching.
    std::string databases;  // comma-separated, e.g. "db1,db2" or "all"
    std::string users;      // comma-separated, e.g. "u1,u2" or "all"
    // Network address (only for host/hostssl/hostnossl).
    // Stored as a CIDR string (e.g. "192.168.1.0/24") or empty for "all".
    std::string cidr;
    // Netmask bits (0 for "all" or for local connections).
    int mask_bits = 0;
    // The auth method.
    HbaMethod method = HbaMethod::kTrust;
    // The line number in pg_hba.conf (1-based, for error messages).
    int lineno = 0;
    // Raw line text (for diagnostics).
    std::string raw;
};

// HbaConfig — the parsed contents of pg_hba.conf.
struct HbaConfig {
    std::vector<HbaLine> lines;
    // Whether parsing succeeded (false if any line had a syntax error).
    bool valid = true;
    // The first error message (if !valid).
    std::string error;
    // The line number of the first error (if !valid).
    int error_lineno = 0;
};

// ParseHbaConfig — parse a pg_hba.conf document.
// `text` is the full file contents (lines separated by '\n').
// On error, returns a config with `valid=false` and the first error.
HbaConfig ParseHbaConfig(const std::string& text);

// SelectHbaLine — pick the first HbaLine matching the given connection.
// `database`, `user` are the requested names. `addr` is the client IP
// (empty for local). `ssl_in_use` is true if the connection is over SSL.
// Returns nullptr if no line matches (caller should reject).
const HbaLine* SelectHbaLine(const HbaConfig& config, const std::string& database,
                             const std::string& user, const std::string& addr, bool ssl_in_use);

// --- Helpers for matching ---

// MatchDatabaseOrUser — true if the token list matches the given name.
// "all" matches anything; "sameuser"/"replication" only match those names.
// A comma-separated list matches if any element matches.
bool MatchDatabaseOrUser(const std::string& token_list, const std::string& name);

// MatchCidr — true if `addr` falls within the CIDR `cidr`/`mask_bits`.
// Both arguments are IPv4 dotted-quad; returns false on parse error.
bool MatchCidr(const std::string& addr, const std::string& cidr, int mask_bits);

// ParseHbaMethod — convert a method name string to enum.
HbaMethod ParseHbaMethod(const std::string& name);

// HbaMethodToString — string representation (for diagnostics).
std::string HbaMethodToString(HbaMethod method);

}  // namespace pgcpp::protocol

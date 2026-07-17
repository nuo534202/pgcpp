// conninfo.hpp — Connection string / URI parsing (P3-11).
//
// Mirrors PostgreSQL's libpq `PQconninfoParse` / `PQconndefaults` APIs.
// Supports two input forms:
//
//   1. Keyword=value list (whitespace separated, values may be quoted):
//        "host=localhost port=5432 user=alice dbname=testdb"
//
//   2. URI (postgresql:// or postgres:// scheme):
//        "postgresql://alice@localhost:5432/testdb?sslmode=disable"
//
// Unknown keywords are preserved (libpq semantics) so callers can read
// extension options. The parser is intentionally permissive about
// whitespace and quoting, matching libpq's fe-connect.c behaviour.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pgcpp::libpq {

// ConnStatusType — connection establishment status (mirrors libpq).
enum class ConnStatusType {
    kOk,                // Connection established.
    kBad,               // Connection failed / not established.
    kStarted,           // Nonblocking connect started.
    kMade,              // Connection made, awaiting startup packet.
    kAwaitingResponse,  // Awaiting server response to startup packet.
    kAwaitingAuth,      // Awaiting authentication reply.
    kPollingWriting,    // Polling for write-ready (async connect).
    kPollingReading,    // Polling for read-ready (async connect).
    kPollingActive,     // Polling in progress (no I/O wait).
};

// PollingStatusType — result of PQconnectPoll (mirrors PostgresPollingStatusType).
enum class PollingStatusType {
    kFailed,   // Connection attempt failed.
    kReading,  // Wait until socket is read-ready.
    kWriting,  // Wait until socket is write-ready.
    kOk,       // Connection complete.
    kActive,   // Internal: still working (no I/O wait).
};

// ConnInfoOption — a single connection parameter.
//
// Mirrors libpq's PQconninfoOption struct. `keyword` is the canonical
// libpq option name (host/port/user/dbname/...), `val` is the resolved
// value (empty string if unset), `label`/`dispchar`/`dispsize` are
// presentation hints for GUI clients (kept for API parity).
struct ConnInfoOption {
    std::string keyword;   // e.g. "host", "port", "user", "dbname"
    std::string val;       // resolved value (empty if unset)
    std::string label;     // display label
    std::string dispchar;  // display hint: "" / "*" (password) / "D" (debug)
    int dispsize = 0;      // suggested display width
};

// ParseConnInfo — parse a connection string (keyword=value or URI form).
//
// On success, fills `out` with one ConnInfoOption per recognized keyword
// (only keywords that appear in the input are emitted; default values
// are not added by this call — use FillDefaults separately). Returns
// true on success; on parse error, returns false and sets `errmsg`.
//
// Behavior matches libpq's conninfo_uri_parse / conninfo_parse.
bool ParseConnInfo(const std::string& conninfo, std::vector<ConnInfoOption>& out,
                   std::string& errmsg);

// FillDefaults — populate missing keywords with libpq defaults.
//
// After ParseConnInfo, callers may invoke this to ensure the result
// vector contains every recognized keyword (host, hostaddr, port, user,
// dbname, etc.) with sensible defaults filled in. Defaults match
// libpq's conndefaults.c: port=5432, user=$PGUSER or current login,
// host=$PGHOST or "localhost".
void FillDefaults(std::vector<ConnInfoOption>& opts);

// GetOption — find a keyword in the option list, returning a pointer to
// its value (or nullptr if not present). Used by callers that want to
// read an option after parsing.
const std::string* GetOption(const std::vector<ConnInfoOption>& opts, const std::string& keyword);

// SetOption — set or replace a keyword's value. If the keyword is not
// present, it is appended.
void SetOption(std::vector<ConnInfoOption>& opts, const std::string& keyword,
               const std::string& val);

// BuildConninfoString — serialize an option list back to the
// keyword=value form (with quoting as needed). Useful for building a
// connection string programmatically.
std::string BuildConninfoString(const std::vector<ConnInfoOption>& opts);

// DefaultUser — return the default user name ($PGUSER or login name).
// Implemented in conninfo.cpp using getenv and getpwuid.
std::string DefaultUser();

// DefaultHost — return the default host ($PGHOST or "localhost").
std::string DefaultHost();

// DefaultPort — return the default port ($PGPORT or 5432).
int DefaultPort();

}  // namespace pgcpp::libpq

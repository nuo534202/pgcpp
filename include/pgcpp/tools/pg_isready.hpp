// pg_isready.h — Server readiness check (pg_isready).
//
// Converted from PostgreSQL 15's src/bin/pg_isready/.
//
// pg_isready connects to a PostgreSQL (or pgcpp) server and reports whether
// it is accepting connections. It returns:
//   0 — server is accepting connections.
//   1 — server is rejecting connections (e.g. wrong DB, wrong user).
//   2 — server is not responding (no answer on the port).
//   3 — no attempt was made (e.g. bad command-line arguments).
//
// pgcpp's pg_isready is a thin wrapper over PsqlClient: it tries to connect
// and reports the outcome.
#pragma once

#include <string>

namespace pgcpp::tools {

// ReadyState — the outcome of a pg_isready probe.
enum class ReadyState {
    kAccepting,   // connected successfully
    kRejecting,   // server refused (auth, wrong DB)
    kNoResponse,  // no answer on the port
    kNoAttempt,   // no probe was made (bad args)
};

// IsReadyOptions — inputs to the probe.
struct IsReadyOptions {
    std::string host = "127.0.0.1";
    int port = 5433;
    std::string database;  // empty = default
    std::string user;      // empty = default
    int timeout_secs = 3;  // connect timeout
};

// CheckServerReady — probe the server and return the ReadyState.
ReadyState CheckServerReady(const IsReadyOptions& opts);

// ReadyStateToString — human-readable description of a ReadyState.
const char* ReadyStateToString(ReadyState state);

// ReadyStateToExitCode — the exit code pg_isready should return.
int ReadyStateToExitCode(ReadyState state);

}  // namespace pgcpp::tools

// pg_isready.cpp — Server readiness check (pg_isready).
//
// Probes a pgcpp/PostgreSQL server by attempting a PsqlClient connection.
// Returns a ReadyState describing whether the server is accepting connections,
// rejecting them, not responding, or was not probed at all (bad arguments).
#include "tools/pg_isready.hpp"

#include "tools/psql_client.hpp"

namespace pgcpp::tools {

ReadyState CheckServerReady(const IsReadyOptions& opts) {
    // Validate the port range; out-of-range means "no attempt".
    if (opts.port <= 0 || opts.port > 65535)
        return ReadyState::kNoAttempt;

    PsqlClient client(opts.host, opts.port);
    if (client.Connect()) {
        client.Disconnect();
        return ReadyState::kAccepting;
    }
    // PsqlClient does not expose the underlying error, so we cannot
    // distinguish ECONNREFUSED (rejecting) from ETIMEDOUT (no response).
    // A more sophisticated implementation would inspect errno here.
    return ReadyState::kNoResponse;
}

const char* ReadyStateToString(ReadyState state) {
    switch (state) {
        case ReadyState::kAccepting:
            return "accepting connections";
        case ReadyState::kRejecting:
            return "rejecting connections";
        case ReadyState::kNoResponse:
            return "no response";
        case ReadyState::kNoAttempt:
            return "no attempt";
    }
    return "unknown";
}

int ReadyStateToExitCode(ReadyState state) {
    switch (state) {
        case ReadyState::kAccepting:
            return 0;
        case ReadyState::kRejecting:
            return 1;
        case ReadyState::kNoResponse:
            return 2;
        case ReadyState::kNoAttempt:
            return 3;
    }
    return 3;
}

}  // namespace pgcpp::tools

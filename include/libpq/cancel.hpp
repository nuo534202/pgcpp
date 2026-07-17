// cancel.hpp — Query cancellation (P3-11).
//
// Mirrors PostgreSQL libpq's PQgetCancel / PQcancel API. A PgCancel
// handle is created from a live PgConn and used to send a cancel
// request to the server on a *separate* short-lived TCP connection
// (matching libpq's behaviour — the cancel request is sent over a
// fresh socket because the main connection may be blocked reading a
// long-running query).
//
// pgcpp's server (postmaster.cpp) accepts cancel requests on the same
// listening port as the regular startup: a request whose first byte
// after the length is 0x04 (CancelRequest) triggers
// ProcessCancelRequest instead of the normal startup handshake.
#pragma once

#include <cstdint>
#include <string>

namespace pgcpp::libpq {

class PgConn;

// PgCancel — opaque cancel handle. Created by GetCancel from a live
// connection. The handle captures the host/port/secret/pid needed to
// formulate a CancelRequest packet; it is independent of the parent
// connection's socket.
struct PgCancel {
    std::string host;
    int port = 0;
    uint32_t pid = 0;     // backend PID
    uint32_t secret = 0;  // secret from BackendKeyData
};

// GetCancel — extract a cancel handle from a live connection.
// Returns true if the connection has the backend key data needed to
// formulate a cancel request, false otherwise.
bool GetCancel(const PgConn& conn, PgCancel& out);

// Cancel — send a cancel request for the connection identified by the
// given cancel handle. Returns true on success (cancel request sent
// and acknowledged by the server), false on error.
//
// Note: A successful return does *not* guarantee that the running query
// was actually cancelled — the server may process the cancel request
// after the query has already finished. The caller should re-check the
// original connection's state.
bool Cancel(const PgCancel& cancel);

}  // namespace pgcpp::libpq

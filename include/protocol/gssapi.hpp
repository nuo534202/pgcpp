// gssapi.h — GSSAPI authentication (degraded mode for pgcpp).
//
// Converted from PostgreSQL 15's src/backend/libpq/be-gssapi.c.
//
// PG uses GSSAPI for Kerberos authentication. The backend receives the
// client's GSS token, delegates to gss_accept_sec_context, and on success
// extracts the principal name (which becomes the authenticated identity).
//
// pgcpp does not link libgssapi_krb5. The GSSAPI auth handler is therefore
// a stub that always reports "GSS not available". The API is preserved so
// the auth.c dispatcher can call into it (and so a real implementation can
// be added later without touching callers).
#pragma once

#include <cstdint>
#include <string>

#include "protocol/pqformat.hpp"

namespace pgcpp::protocol {

// GssAuthResult — outcome of a GSSAPI authentication exchange.
enum class GssAuthResult {
    kSuccess,
    kNotAvailable,  // GSSAPI not compiled in (always returned by pgcpp)
    kContinue,      // another round-trip is needed
    kFailure,       // authentication failed
};

// GssContext — per-connection GSSAPI state.
struct GssContext {
    bool in_progress = false;
    std::string principal;  // authenticated principal (on success)
    // Number of round-trips performed.
    int round_trips = 0;
};

// pg_GSS_recvauth — drive the GSSAPI accept_sec_context loop.
// Reads client tokens via TakeMockClientResponse (tests) or the socket
// (production). Returns kNotAvailable in pgcpp.
GssAuthResult pg_GSS_recvauth(OutputSink* sink, GssContext& ctx);

// secure_open_gssapi — initialise GSS on an accepted socket.
// Returns false in pgcpp (no GSSAPI support).
bool secure_open_gssapi(int fd);

// IsGssEnabled — true if the server was configured with gss=on.
// Always returns false in pgcpp.
bool IsGssEnabled();

// GetGssPrincipal — return the authenticated principal for a connection.
// Returns empty if GSS is not in use.
std::string GetGssPrincipal(int fd);

// ResetGssState — clear all GSS state (used by tests).
void ResetGssState();

}  // namespace pgcpp::protocol

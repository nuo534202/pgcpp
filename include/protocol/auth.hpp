// auth.h — Client authentication (dispatch on pg_hba.conf method).
//
// Converted from PostgreSQL 15's src/backend/libpq/auth.c.
//
// ClientAuthentication is the entry point invoked by the backend after the
// startup packet is received. It looks up the matching pg_hba.conf line,
// then drives the appropriate authentication exchange:
//   - trust:    succeed immediately
//   - password: receive a PasswordMessage and verify with crypt.c
//   - md5:      send AuthenticationMD5Password, receive response, verify
//   - scram:    send AuthenticationSASL, run SCRAM-SHA-256 exchange
//   - gss:      delegate to gssapi.c
//   - reject:   always fail
//
// The C port separates "send auth request" and "receive response" into
// be-secure.c calls; the C++ port uses the OutputSink abstraction.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "protocol/crypt.hpp"
#include "protocol/hba.hpp"
#include "protocol/pqformat.hpp"

namespace pgcpp::protocol {

// AuthRequest — authentication request codes sent in Authentication messages
// (type 'R', payload: int32 code [+ optional extra data]).
// Mirrors PG's AUTH_REQ_* constants.
enum class AuthRequest : int32_t {
    kOk = 0,             // AuthenticationOk — authentication complete
    kKrb4 = 1,           // (obsolete)
    kKrb5 = 2,           // (obsolete)
    kPassword = 3,       // cleartext password required
    kCrypt = 4,          // (obsolete)
    kMd5 = 5,            // md5(password+salt) required
    kScramSha256 = 10,   // SCRAM-SHA-256 SASL (advertise mechanisms)
    kGss = 7,            // GSSAPI required
    kGssCont = 8,        // GSSAPI continuation (payload)
    kSspi = 9,           // (Windows only)
    kSaslContinue = 11,  // AuthenticationSASLContinue — server-first message
    kSaslFinal = 12,     // AuthenticationSASLFinal — server-final message
};

// ResponseReader — function that reads one client response (PasswordMessage
// or SASLResponse) for the auth exchange. In production this reads from the
// client socket; in tests it's typically TakeMockClientResponse.
using ResponseReader = std::function<std::string()>;

// AuthResult — outcome of an authentication attempt.
enum class AuthResult {
    kSuccess,
    kWrongPassword,
    kNoSuchUser,
    kRejected,           // pg_hba.conf reject
    kMethodUnsupported,  // method not implemented in pgcpp
    kProtocolError,      // malformed message from client
};

// AuthContext — inputs to ClientAuthentication.
struct AuthContext {
    // The user name from the startup packet.
    std::string user;
    // The matching pg_hba.conf line (selected by hba.c).
    HbaLine hba_line;
    // The stored password hash for the user (empty if no user/none stored).
    // Filled in by the caller from pg_authid.
    std::string stored_password;
    // Output sink for sending auth requests to the client.
    OutputSink* sink = nullptr;
};

// ClientAuthentication — drive the authentication exchange.
// Returns AuthResult::kSuccess on success; otherwise an error response is
// sent to the client and the caller should close the connection.
AuthResult ClientAuthentication(const AuthContext& ctx);

// SendAuthRequest — send an Authentication message (type 'R') to the client.
// `code` is the AuthRequest code; `extra` is optional payload (e.g. the MD5
// salt or SASL mechanism list).
void SendAuthRequest(OutputSink* sink, AuthRequest code, const std::string& extra = "");

// SendErrorResponse — send an ErrorResponse message and flush.
// `code` is the SQLSTATE; `message` is the human-readable error.
void SendErrorResponse(OutputSink* sink, const std::string& code, const std::string& message);

// --- Method-specific handlers (exposed for testing) ---

// CheckTrustAuth — always succeeds (pg_hba method = trust).
AuthResult CheckTrustAuth();

// CheckPasswordAuth — receive a PasswordMessage and verify with crypt.c.
// `stored_password` is the raw shadow password from pg_authid.
AuthResult CheckPasswordAuth(OutputSink* sink, const std::string& user,
                             const std::string& stored_password);

// CheckMd5Auth — send AuthenticationMd5Password, receive response, verify.
// The salt is a 4-byte random value chosen by the server.
AuthResult CheckMd5Auth(OutputSink* sink, const std::string& user,
                        const std::string& stored_password, uint32_t salt);

// CheckScramAuth — send AuthenticationSASL, run the SCRAM-SHA-256 exchange.
// Implements RFC 5802: client-first → server-first → client-final →
// server-final. The stored password must be a SCRAM-SHA-256 hash
// (produced by EncryptPassword with kScramSha256).
AuthResult CheckScramAuth(OutputSink* sink, const std::string& user,
                          const std::string& stored_password);

// CheckRejectAuth — always fails (pg_hba method = reject).
AuthResult CheckRejectAuth();

// --- Test helpers for injecting client responses ---
//
// In PG, the auth exchange reads bytes from the client socket. pgcpp
// separates "what the client sends" via a per-thread queue so tests can
// inject password messages deterministically.

// SetMockClientResponse — push a pre-built message body that the next auth
// handler will read as the client's response. Multiple calls queue multiple
// responses. Used by tests; in production the bytes come from the socket.
void SetMockClientResponse(const std::string& payload);

// TakeMockClientResponse — consume one queued response (or "" if none).
std::string TakeMockClientResponse();

// ClearMockClientResponses — discard all queued responses.
void ClearMockClientResponses();

// SetGlobalResponseReader — install a process-wide response reader used by
// the auth handlers' internal ReadPasswordMessage. In production
// (BackendMain) this is set to a lambda that reads from the client socket.
// Pass a default-constructed (empty) ResponseReader to clear, which makes
// the handlers fall back to the mock queue (test mode).
void SetGlobalResponseReader(ResponseReader reader);

}  // namespace pgcpp::protocol

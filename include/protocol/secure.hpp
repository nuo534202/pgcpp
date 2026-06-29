// secure.h — Secure transport layer (SSL/TLS pass-through for pgcpp).
//
// Converted from PostgreSQL 15's src/backend/libpq/be-secure.c and
// src/backend/libpq/be-secure-openssl.c.
//
// PG wraps the client socket in an SSL/TLS layer when ssl=on and the client
// requests SSL during startup. The `secure_*` functions are the indirection
// layer: every socket read/write goes through them, and they either dispatch
// to the SSL backend (OpenSSL) or fall through to plain read/write.
//
// pgcpp does not link OpenSSL. The secure layer is therefore a pass-through:
// `secure_initialize` is a no-op, `secure_open_server` reports that SSL is
// unavailable, and `secure_read`/`secure_write` call plain read/write. The
// API is preserved so that the rest of the code can use the same call sites
// as PG (and so that SSL can be plugged in later without touching callers).
#pragma once

#include <cstdint>
#include <string>

namespace pgcpp::protocol {

// SecureLayer — indicates which transport is in use for a connection.
enum class SecureLayer {
    kPlain,  // no SSL
    kSsl,    // SSL/TLS (not implemented in pgcpp)
    kGss,    // GSSAPI (not implemented in pgcpp)
};

// SecureInitResult — outcome of secure_initialize.
struct SecureInitResult {
    bool ok = true;
    std::string error;
};

// SecureStatus — per-connection state of the secure layer.
struct SecureStatus {
    SecureLayer layer = SecureLayer::kPlain;
    bool initialized = false;
    // For SSL: the negotiated cipher suite (empty for plain).
    std::string cipher;
    // For SSL: the negotiated protocol version (empty for plain).
    std::string protocol_version;
};

// secure_initialize — initialise the secure transport (load certs, etc.).
// In pgcpp this is a no-op that always succeeds.
SecureInitResult secure_initialize();

// secure_open_server — negotiate SSL/TLS on an accepted socket.
// In pgcpp this always returns "SSL not available" (the caller falls back
// to plain TCP).
SecureInitResult secure_open_server(int fd);

// secure_close — tear down the secure layer (no-op for plain).
void secure_close(int fd);

// secure_read — read up to `len` bytes into `buf` from the connection.
// Returns the number of bytes read (>=0) or -1 on error.
// In pgcpp this is a plain read(2).
long secure_read(int fd, void* buf, size_t len);

// secure_write — write `len` bytes from `buf` to the connection.
// Returns the number of bytes written (>=0) or -1 on error.
// In pgcpp this is a plain write(2).
long secure_write(int fd, const void* buf, size_t len);

// IsSslEnabled — true if the server was configured with ssl=on.
// In pgcpp this always returns false (no SSL support compiled in).
bool IsSslEnabled();

// GetSecureStatus — return the per-connection secure status.
SecureStatus GetSecureStatus(int fd);

// ResetSecureState — clear all per-connection state (used by tests).
void ResetSecureState();

}  // namespace pgcpp::protocol

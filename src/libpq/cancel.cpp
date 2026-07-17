// cancel.cpp — Query cancellation implementation (P3-11).
//
// Mirrors libpq's PQgetCancel / PQcancel. A cancel request is a special
// startup-class message sent over a fresh TCP connection (the original
// connection may be blocked waiting for the long-running query). The
// packet format is:
//
//   int32  length (16, includes self)
//   int32  CancelRequestCode (0x04D2162F, libpq constant)
//   int32  PID (backend PID from BackendKeyData)
//   int32  secret (secret from BackendKeyData)
//
// pgcpp's postmaster accepts CancelRequest packets on the same listening
// port as regular startup: a request whose first int32 after the length
// is CancelRequestCode triggers ProcessCancelRequest instead of the
// normal startup handshake.
#include "libpq/cancel.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "libpq/libpq.hpp"

namespace pgcpp::libpq {

namespace {

// Write a 4-byte int32 in network byte order into `buf` at offset.
void PutInt32(char* buf, int32_t v) {
    uint32_t u = static_cast<uint32_t>(v);
    buf[0] = static_cast<char>((u >> 24) & 0xFF);
    buf[1] = static_cast<char>((u >> 16) & 0xFF);
    buf[2] = static_cast<char>((u >> 8) & 0xFF);
    buf[3] = static_cast<char>(u & 0xFF);
}

}  // namespace

bool GetCancel(const PgConn& conn, PgCancel& out) {
    if (conn.Status() != ConnStatusType::kOk) {
        return false;
    }
    if (conn.BackendPID() == 0) {
        // No BackendKeyData received — cannot formulate cancel request.
        return false;
    }
    out.host = conn.Host();
    out.port = conn.Port();
    out.pid = static_cast<uint32_t>(conn.BackendPID());
    out.secret = conn.BackendSecret();
    return true;
}

bool Cancel(const PgCancel& cancel) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return false;

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(cancel.port));
    if (inet_pton(AF_INET, cancel.host.c_str(), &addr.sin_addr) <= 0) {
        close(fd);
        return false;
    }
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return false;
    }

    // Build the 16-byte CancelRequest packet.
    char msg[16];
    PutInt32(msg, 16);              // length (includes self)
    PutInt32(msg + 4, 0x04D2162F);  // CancelRequestCode
    PutInt32(msg + 8, static_cast<int32_t>(cancel.pid));
    PutInt32(msg + 12, static_cast<int32_t>(cancel.secret));

    std::size_t written = 0;
    while (written < sizeof(msg)) {
        ssize_t n = write(fd, msg + written, sizeof(msg) - written);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return false;
        }
        if (n == 0) {
            close(fd);
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    // We do not read any response: the server closes the cancel socket
    // immediately after processing the request. Close our side.
    close(fd);
    return true;
}

}  // namespace pgcpp::libpq

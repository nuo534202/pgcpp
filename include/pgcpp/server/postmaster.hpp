// postmaster.h — Postmaster: server process that accepts connections and
// forks backend processes.
//
// Converted from PostgreSQL 15's src/backend/postmaster/postmaster.c.
//
// The postmaster is the main server process. It:
//   1. Initializes the global subsystems (catalog, storage, transactions).
//   2. Listens on a TCP port for client connections.
//   3. Forks a child process for each accepted connection.
//   4. Each child runs BackendMain to handle the frontend protocol.
//   5. Reaps exited child processes (SIGCHLD).
//   6. Shuts down gracefully on SIGTERM/SIGINT.
//
// The fork model is preserved from PostgreSQL per AGENTS.md constraints:
// no std::thread, no C++ exceptions.
#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "mytoydb/protocol/pqformat.hpp"

namespace mytoydb::server {

// ServerConfig — configuration for the postmaster.
struct ServerConfig {
    std::string data_dir;                   // data directory path
    int port = 5433;                        // TCP listen port (avoid clashing with real PG)
    int max_connections = 100;              // maximum concurrent backend processes
    std::string listen_addr = "127.0.0.1";  // bind address
};

// Special startup packet protocol codes (network-byte-order int32 in the
// first 4 bytes of the startup packet, where a v3.0 packet would carry
// 0x00030000). See src/backend/postmaster/postmaster.c.
constexpr uint32_t kNegotiateSslCode = 80877103;   // SSLRequest
constexpr uint32_t kNegotiateGssCode = 80877104;   // GSSENCRequest
constexpr uint32_t kCancelRequestCode = 80877102;  // CancelRequest

// StartupPacketResult — outcome of ProcessStartupPacket.
struct StartupPacketResult {
    // True if a valid v3.0 startup packet was received and parsed.
    // False on EOF/error, or on a CancelRequest (which the caller should
    // silently drop), or on any unrecoverable protocol violation.
    bool valid = false;

    // The parsed user name (empty if not supplied by the client).
    std::string user;

    // The parsed database name. Defaults to `user` when the client does not
    // supply a `database` parameter (matching PostgreSQL's behavior).
    std::string database;

    // The application_name parameter if supplied.
    std::string application_name;

    // All key-value pairs parsed from the startup packet, in arrival order.
    // (Stored in a map for convenient lookup; duplicates keep the last value.)
    std::map<std::string, std::string> options;

    // The protocol version reported by the client (e.g. 0x00030000 for v3.0).
    uint32_t protocol_version = 0;
};

// ProcessStartupPacket — read and parse the frontend startup packet sequence.
//
// Handles the optional SSLRequest / GSSENCRequest preambles (responds 'N'
// — no SSL/GSS — and continues reading) and the CancelRequest message
// (returns valid=false; the caller should close the connection without
// further protocol traffic).
//
// On a v3.0 startup packet, parses the null-terminated key/value pairs and
// returns them in `result.options` (with `user`/`database`/`application_name`
// mirrored into dedicated fields for convenience). `database` defaults to
// `user` when not explicitly supplied.
//
// `client_fd` is the accepted TCP socket. On EOF or read error, returns a
// result with valid=false.
StartupPacketResult ProcessStartupPacket(int client_fd);

// SocketSink — OutputSink that writes protocol messages to a socket fd.
//
// Each SendMessage call writes the full wire-format message (type byte +
// 4-byte length + payload) to the socket. Writes are blocking.
class SocketSink : public mytoydb::protocol::OutputSink {
public:
    explicit SocketSink(int fd);
    ~SocketSink() override;

    SocketSink(const SocketSink&) = delete;
    SocketSink& operator=(const SocketSink&) = delete;

    void SendMessage(const mytoydb::protocol::Message& msg) override;

    int fd() const { return fd_; }

private:
    int fd_;
};

// Postmaster — the main server process.
//
// Manages the listening socket, accepts connections, and forks backends.
// Signal handling: SIGTERM/SIGINT trigger graceful shutdown;
// SIGCHLD reaps zombie children.
class Postmaster {
public:
    explicit Postmaster(ServerConfig config);
    ~Postmaster();

    Postmaster(const Postmaster&) = delete;
    Postmaster& operator=(const Postmaster&) = delete;

    // Run the server loop. Blocks until shutdown is requested.
    // Returns 0 on clean shutdown, non-zero on error.
    int Run();

    // Request shutdown (signal-safe).
    void RequestShutdown();

    // Get the listen port (useful when port=0 was specified).
    int listen_port() const { return listen_port_; }

private:
    // Initialize global subsystems (catalog, storage, transactions, buffer pool).
    void InitializeSubsystems();

    // Create the listening socket and bind it.
    void SetupListenSocket();

    // Accept one connection and fork a backend.
    // Returns child PID in the parent, 0 in the child, -1 on error.
    int AcceptAndFork();

    // Reap exited child processes.
    void ReapChildren();

    ServerConfig config_;
    int listen_fd_ = -1;
    int listen_port_ = 0;
    volatile bool shutdown_requested_ = false;
    int active_children_ = 0;
};

// BackendMain — the per-connection backend loop.
//
// Reads protocol messages from the client socket and dispatches them to
// the protocol handler (mytoydb::protocol::Backend). Runs until the client
// sends a Terminate message or disconnects.
//
// This is the entry point for a forked child process.
void BackendMain(int client_fd);

// InitializeServerSubsystems — initialize all global subsystems needed
// for the server to run. Called once at startup (in postmaster and
// inherited by forked children).
void InitializeServerSubsystems(const std::string& data_dir);

// ShutdownServerSubsystems — clean up global subsystems.
void ShutdownServerSubsystems();

}  // namespace mytoydb::server

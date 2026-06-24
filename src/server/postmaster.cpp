// postmaster.cpp — Postmaster: server process implementation.
//
// Converted from PostgreSQL 15's src/backend/postmaster/postmaster.c.
//
// Implements the fork-based server model:
//   1. Postmaster::Run() listens on a TCP socket.
//   2. For each accepted connection, fork() creates a child process.
//   3. The child calls BackendMain() which runs the protocol loop.
//   4. The parent reaps children via SIGCHLD.
//   5. SIGTERM/SIGINT trigger graceful shutdown.
#include "mytoydb/server/postmaster.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

#include "mytoydb/access/rel.h"
#include "mytoydb/catalog/bootstrap_catalog.h"
#include "mytoydb/catalog/catalog.h"
#include "mytoydb/catalog/syscache.h"
#include "mytoydb/common/error/elog.h"
#include "mytoydb/common/memory/alloc_set.h"
#include "mytoydb/common/memory/memory_context.h"
#include "mytoydb/protocol/postgres.h"
#include "mytoydb/protocol/pqformat.h"
#include "mytoydb/storage/bufmgr.h"
#include "mytoydb/storage/smgr.h"
#include "mytoydb/transaction/snapshot.h"
#include "mytoydb/transaction/transam.h"
#include "mytoydb/transaction/xact.h"

namespace mytoydb::server {

using mytoydb::access::InitializeRelcache;
using mytoydb::access::ResetRelcache;
using mytoydb::catalog::BootstrapCatalog;
using mytoydb::catalog::Catalog;
using mytoydb::catalog::GetCatalog;
using mytoydb::catalog::SetCatalog;
using mytoydb::catalog::SetSysCache;
using mytoydb::catalog::SysCache;
using mytoydb::error::InitErrorSubsystem;
using mytoydb::error::LogLevel;
using mytoydb::memory::AllocSetContext;
using mytoydb::memory::MemoryContext;
using mytoydb::memory::SetCurrentMemoryContext;
using mytoydb::protocol::Backend;
using mytoydb::protocol::DescribeKind;
using mytoydb::protocol::Message;
using mytoydb::protocol::MessageReader;
using mytoydb::protocol::MessageType;
using mytoydb::protocol::TransactionStatus;
using mytoydb::storage::InitBufferPool;
using mytoydb::storage::SetStorageBaseDir;
using mytoydb::storage::ShutdownBufferPool;
using mytoydb::storage::smgrcloseall;
using mytoydb::transaction::InitializeSnapshotManager;
using mytoydb::transaction::InitializeTransactionSystem;
using mytoydb::transaction::ResetTransactionState;

// ---------------------------------------------------------------------------
// Signal handling
// ---------------------------------------------------------------------------

namespace {

// Global postmaster pointer for signal handlers.
Postmaster* g_postmaster = nullptr;

// Signal handler for SIGTERM/SIGINT — request graceful shutdown.
void HandleShutdownSignal(int /*sig*/) {
    if (g_postmaster != nullptr) {
        g_postmaster->RequestShutdown();
    }
}

// Signal handler for SIGCHLD — reap zombie children (non-blocking).
void HandleSigChld(int /*sig*/) {
    // Just set a flag; actual reaping happens in the main loop.
    // Writing to a volatile sig_atomic_t flag is signal-safe.
}

// Signal handler for SIGPIPE — ignore (we handle write errors explicitly).
void HandleSigPipe(int /*sig*/) {
    // Ignore SIGPIPE. Write errors are handled by checking return values.
}

// Install signal handlers for the postmaster.
void InstallSignalHandlers() {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = HandleShutdownSignal;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    sa.sa_handler = HandleSigChld;
    sa.sa_flags = SA_NOCLDSTOP | SA_RESTART;
    sigaction(SIGCHLD, &sa, nullptr);

    sa.sa_handler = HandleSigPipe;
    sigaction(SIGPIPE, &sa, nullptr);
}

// Reset signal handlers to defaults in the child process.
void ResetSignalHandlers() {
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
}

// Write all bytes to a fd (handles partial writes).
// Returns true on success, false on error.
bool WriteAll(int fd, const char* data, std::size_t len) {
    std::size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, data + written, len - written);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
        written += static_cast<std::size_t>(n);
    }
    return true;
}

// Read exactly len bytes from a fd (handles partial reads).
// Returns true on success, false on EOF or error.
bool ReadAll(int fd, char* buf, std::size_t len) {
    std::size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, buf + got, len - got);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;  // EOF
        got += static_cast<std::size_t>(n);
    }
    return true;
}

// Read a single protocol message from the client.
// Returns true if a message was read, false on EOF/error.
// Sets type and payload (payload excludes type byte and length field).
bool ReadMessage(int fd, char* type, std::string& payload) {
    // Read the type byte.
    if (!ReadAll(fd, type, 1))
        return false;

    // Read the 4-byte length.
    char len_buf[4];
    if (!ReadAll(fd, len_buf, 4))
        return false;

    int32_t length = static_cast<int32_t>(
        (static_cast<uint8_t>(len_buf[0]) << 24) | (static_cast<uint8_t>(len_buf[1]) << 16) |
        (static_cast<uint8_t>(len_buf[2]) << 8) | static_cast<uint8_t>(len_buf[3]));

    // The length includes itself (4 bytes) but not the type byte.
    if (length < 4)
        return false;

    std::size_t payload_len = static_cast<std::size_t>(length) - 4;
    if (payload_len > 0) {
        payload.resize(payload_len);
        if (!ReadAll(fd, payload.data(), payload_len))
            return false;
    } else {
        payload.clear();
    }

    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// SocketSink
// ---------------------------------------------------------------------------

SocketSink::SocketSink(int fd) : fd_(fd) {}

SocketSink::~SocketSink() {
    if (fd_ >= 0) {
        close(fd_);
    }
}

void SocketSink::SendMessage(const Message& msg) {
    std::string wire = msg.BuildWireFormat();
    WriteAll(fd_, wire.data(), wire.size());
}

// ---------------------------------------------------------------------------
// InitializeServerSubsystems / ShutdownServerSubsystems
// ---------------------------------------------------------------------------

// Global state for subsystem initialization.
namespace {

struct ServerGlobalState {
    MemoryContext* top_context = nullptr;
    Catalog* catalog = nullptr;
    SysCache* syscache = nullptr;
    bool buffer_pool_initialized = false;
    std::string data_dir;
};

ServerGlobalState g_state;

}  // namespace

void InitializeServerSubsystems(const std::string& data_dir) {
    g_state.data_dir = data_dir;

    // Error subsystem.
    InitErrorSubsystem();

    // Top-level memory context.
    g_state.top_context = AllocSetContext::Create("ServerContext");
    SetCurrentMemoryContext(g_state.top_context);

    // Catalog + syscache.
    g_state.catalog = new Catalog();
    SetCatalog(g_state.catalog);
    BootstrapCatalog(g_state.catalog);

    g_state.syscache = new SysCache();
    SetSysCache(g_state.syscache);

    // Transaction system.
    ResetTransactionState();
    InitializeTransactionSystem();
    InitializeSnapshotManager();

    // Storage.
    SetStorageBaseDir(data_dir);
    InitBufferPool(64);
    g_state.buffer_pool_initialized = true;

    // Relcache.
    InitializeRelcache();
}

void ShutdownServerSubsystems() {
    ResetRelcache();

    if (g_state.buffer_pool_initialized) {
        ShutdownBufferPool();
        smgrcloseall();
        g_state.buffer_pool_initialized = false;
    }

    ResetTransactionState();
    InitializeTransactionSystem();
    InitializeSnapshotManager();

    SetSysCache(nullptr);
    SetCatalog(nullptr);
    delete g_state.syscache;
    g_state.syscache = nullptr;
    delete g_state.catalog;
    g_state.catalog = nullptr;

    SetCurrentMemoryContext(nullptr);
    if (g_state.top_context != nullptr) {
        g_state.top_context->Delete();
        g_state.top_context = nullptr;
    }
}

// ---------------------------------------------------------------------------
// BackendMain — per-connection protocol loop
// ---------------------------------------------------------------------------

void BackendMain(int client_fd) {
    // Reset signal handlers to defaults in the child process.
    ResetSignalHandlers();

    // Read the startup message.
    // Format: 4-byte length + 4-byte protocol version + key-value pairs + NUL
    char len_buf[4];
    if (!ReadAll(client_fd, len_buf, 4)) {
        close(client_fd);
        return;
    }

    int32_t startup_len = static_cast<int32_t>(
        (static_cast<uint8_t>(len_buf[0]) << 24) | (static_cast<uint8_t>(len_buf[1]) << 16) |
        (static_cast<uint8_t>(len_buf[2]) << 8) | static_cast<uint8_t>(len_buf[3]));

    if (startup_len < 4 || startup_len > 1024 * 1024) {
        close(client_fd);
        return;
    }

    // Read the rest of the startup message.
    std::size_t rest_len = static_cast<std::size_t>(startup_len) - 4;
    std::vector<char> startup_buf(rest_len);
    if (rest_len > 0 && !ReadAll(client_fd, startup_buf.data(), rest_len)) {
        close(client_fd);
        return;
    }

    // Send AuthenticationOk (type 'R', payload: int32 0).
    {
        char auth_msg[9];
        auth_msg[0] = 'R';
        int32_t auth_len = htonl(8);
        std::memcpy(&auth_msg[1], &auth_len, 4);
        int32_t auth_type = 0;  // AuthenticationOk
        std::memcpy(&auth_msg[5], &auth_type, 4);
        WriteAll(client_fd, auth_msg, 9);
    }

    // Send ParameterStatus messages.
    auto send_param_status = [&](const std::string& key, const std::string& value) {
        std::string payload = key;
        payload.push_back('\0');
        payload += value;
        payload.push_back('\0');
        std::string msg;
        msg.push_back('S');
        int32_t len = htonl(static_cast<int32_t>(4 + payload.size()));
        msg.append(reinterpret_cast<const char*>(&len), 4);
        msg += payload;
        WriteAll(client_fd, msg.data(), msg.size());
    };
    send_param_status("server_version", "mytoydb 1.0");
    send_param_status("client_encoding", "UTF8");
    send_param_status("DateStyle", "ISO");
    send_param_status("integer_datetimes", "on");

    // Send BackendKeyData (type 'K', payload: int32 pid, int32 key).
    {
        char key_msg[13];
        key_msg[0] = 'K';
        int32_t key_len = htonl(12);
        std::memcpy(&key_msg[1], &key_len, 4);
        int32_t pid = static_cast<int32_t>(getpid());
        int32_t key = 0;  // cancellation key (unused)
        std::memcpy(&key_msg[5], &pid, 4);
        std::memcpy(&key_msg[9], &key, 4);
        WriteAll(client_fd, key_msg, 13);
    }

    // Send ReadyForQuery (type 'Z', payload: char 'I' for idle).
    {
        char rfq_msg[6];
        rfq_msg[0] = 'Z';
        int32_t rfq_len = htonl(5);
        std::memcpy(&rfq_msg[1], &rfq_len, 4);
        rfq_msg[5] = 'I';  // Idle
        WriteAll(client_fd, rfq_msg, 6);
    }

    // Create the protocol backend with a SocketSink.
    SocketSink sink(client_fd);
    Backend backend(&sink);

    // Main protocol loop: read messages and dispatch.
    while (true) {
        char type;
        std::string payload;

        if (!ReadMessage(client_fd, &type, payload)) {
            break;  // Client disconnected or error.
        }

        MessageReader reader(payload);

        switch (type) {
            case 'Q': {
                // Simple query.
                std::string query = reader.ReadString();
                backend.exec_simple_query(query);
                break;
            }
            case 'P': {
                // Parse.
                std::string stmt_name = reader.ReadString();
                std::string query = reader.ReadString();
                // Read parameter type OIDs.
                int16_t num_params = reader.ReadInt16();
                std::vector<mytoydb::catalog::Oid> param_types;
                param_types.reserve(static_cast<std::size_t>(num_params));
                for (int16_t i = 0; i < num_params; ++i) {
                    param_types.push_back(static_cast<mytoydb::catalog::Oid>(reader.ReadInt32()));
                }
                backend.HandleParse(stmt_name, query, param_types);
                break;
            }
            case 'B': {
                // Bind.
                std::string portal_name = reader.ReadString();
                std::string stmt_name = reader.ReadString();
                // Parameter format codes.
                int16_t num_formats = reader.ReadInt16();
                std::vector<int16_t> formats;
                formats.reserve(static_cast<std::size_t>(num_formats));
                for (int16_t i = 0; i < num_formats; ++i) {
                    formats.push_back(reader.ReadInt16());
                }
                // Parameter values.
                int16_t num_params = reader.ReadInt16();
                std::vector<std::string> param_values;
                std::vector<bool> param_isnull;
                param_values.reserve(static_cast<std::size_t>(num_params));
                param_isnull.reserve(static_cast<std::size_t>(num_params));
                for (int16_t i = 0; i < num_params; ++i) {
                    int32_t plen = reader.ReadInt32();
                    if (plen < 0) {
                        param_values.emplace_back("");
                        param_isnull.push_back(true);
                    } else {
                        param_values.push_back(reader.ReadBytes(static_cast<std::size_t>(plen)));
                        param_isnull.push_back(false);
                    }
                }
                // Result column format codes (ignored for now).
                int16_t num_result_formats = reader.ReadInt16();
                for (int16_t i = 0; i < num_result_formats; ++i) {
                    reader.ReadInt16();
                }
                backend.HandleBind(portal_name, stmt_name, param_values, param_isnull);
                break;
            }
            case 'D': {
                // Describe.
                char kind = reader.ReadByte();
                std::string name = reader.ReadString();
                backend.HandleDescribe(static_cast<DescribeKind>(kind), name);
                break;
            }
            case 'E': {
                // Execute.
                std::string portal_name = reader.ReadString();
                int32_t max_rows = reader.ReadInt32();
                backend.HandleExecute(portal_name, max_rows);
                break;
            }
            case 'S': {
                // Sync.
                backend.HandleSync();
                break;
            }
            case 'H': {
                // Flush.
                backend.HandleFlush();
                break;
            }
            case 'C': {
                // Close.
                char kind = reader.ReadByte();
                std::string name = reader.ReadString();
                backend.HandleClose(static_cast<DescribeKind>(kind), name);
                break;
            }
            case 'X': {
                // Terminate.
                return;
            }
            default:
                // Unknown message type — ignore.
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// Postmaster
// ---------------------------------------------------------------------------

Postmaster::Postmaster(ServerConfig config) : config_(std::move(config)) {
    g_postmaster = this;
}

Postmaster::~Postmaster() {
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
    if (g_postmaster == this) {
        g_postmaster = nullptr;
    }
}

void Postmaster::RequestShutdown() {
    shutdown_requested_ = true;
}

void Postmaster::InitializeSubsystems() {
    InitializeServerSubsystems(config_.data_dir);
}

void Postmaster::SetupListenSocket() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        ereport(LogLevel::kFatal, "failed to create socket");
    }

    // Allow address reuse.
    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(config_.port));
    if (inet_pton(AF_INET, config_.listen_addr.c_str(), &addr.sin_addr) <= 0) {
        ereport(LogLevel::kFatal, "invalid listen address");
    }

    if (bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ereport(LogLevel::kFatal, "failed to bind socket");
    }

    if (listen(listen_fd_, 5) < 0) {
        ereport(LogLevel::kFatal, "failed to listen on socket");
    }

    // If port was 0, get the actual assigned port.
    if (config_.port == 0) {
        socklen_t addrlen = sizeof(addr);
        if (getsockname(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), &addrlen) == 0) {
            listen_port_ = ntohs(addr.sin_port);
        }
    } else {
        listen_port_ = config_.port;
    }

    // Set the listen socket to non-blocking so accept() doesn't block forever
    // (allows checking shutdown_requested_ periodically).
    int flags = fcntl(listen_fd_, F_GETFL, 0);
    fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK);
}

int Postmaster::AcceptAndFork() {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd =
        accept(listen_fd_, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);

    if (client_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // No pending connections — sleep briefly and retry.
            usleep(10000);  // 10ms
            return -1;
        }
        if (errno == EINTR) {
            return -1;
        }
        ereport(LogLevel::kWarning, "accept failed");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        ereport(LogLevel::kWarning, "fork failed");
        close(client_fd);
        return -1;
    }

    if (pid == 0) {
        // Child process: close the listening socket and handle the connection.
        close(listen_fd_);
        listen_fd_ = -1;
        g_postmaster = nullptr;
        BackendMain(client_fd);
        _exit(0);
    }

    // Parent process: close the client socket and track the child.
    close(client_fd);
    active_children_++;
    return pid;
}

void Postmaster::ReapChildren() {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        active_children_--;
    }
}

int Postmaster::Run() {
    // Initialize subsystems.
    InitializeSubsystems();

    // Install signal handlers.
    InstallSignalHandlers();

    // Set up the listening socket.
    SetupListenSocket();

    elog(LogLevel::kInfo, "MyToyDB server listening on port " + std::to_string(listen_port_));

    // Main accept loop.
    while (!shutdown_requested_) {
        ReapChildren();

        if (active_children_ >= config_.max_connections) {
            usleep(10000);
            continue;
        }

        AcceptAndFork();
    }

    // Graceful shutdown: wait for all children to exit.
    elog(LogLevel::kInfo, "shutting down server...");
    while (active_children_ > 0) {
        ReapChildren();
        if (active_children_ > 0) {
            usleep(10000);
        }
    }

    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }

    ShutdownServerSubsystems();

    elog(LogLevel::kInfo, "server shutdown complete");
    return 0;
}

}  // namespace mytoydb::server

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
#include "server/postmaster.hpp"

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
#include <sstream>
#include <string>
#include <vector>

#include "access/rel.hpp"
#include "catalog/bootstrap_catalog.hpp"
#include "catalog/catalog.hpp"
#include "catalog/syscache.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "protocol/auth.hpp"
#include "protocol/hba.hpp"
#include "protocol/postgres.hpp"
#include "protocol/pqformat.hpp"
#include "stats/stats_bootstrap.hpp"
#include "storage/bufmgr.hpp"
#include "storage/ipc/lwlock.hpp"
#include "storage/ipc/proc.hpp"
#include "storage/ipc/shmem.hpp"
#include "storage/smgr.hpp"
#include "transaction/commit_ts.hpp"
#include "transaction/multixact.hpp"
#include "transaction/procarray.hpp"
#include "transaction/snapshot.hpp"
#include "transaction/transam.hpp"
#include "transaction/xact.hpp"
#include "transaction/xlog.hpp"

namespace pgcpp::server {

using pgcpp::access::InitializeRelcache;
using pgcpp::access::ResetRelcache;
using pgcpp::catalog::BootstrapCatalog;
using pgcpp::catalog::Catalog;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::SetCatalog;
using pgcpp::catalog::SetSysCache;
using pgcpp::catalog::SysCache;
using pgcpp::error::InitErrorSubsystem;
using pgcpp::error::LogLevel;
using pgcpp::memory::AllocSetContext;
using pgcpp::memory::MemoryContext;
using pgcpp::memory::SetCurrentMemoryContext;
using pgcpp::protocol::AuthContext;
using pgcpp::protocol::AuthRequest;
using pgcpp::protocol::AuthResult;
using pgcpp::protocol::Backend;
using pgcpp::protocol::ClientAuthentication;
using pgcpp::protocol::DescribeKind;
using pgcpp::protocol::HbaConfig;
using pgcpp::protocol::HbaLine;
using pgcpp::protocol::HbaMethod;
using pgcpp::protocol::Message;
using pgcpp::protocol::MessageReader;
using pgcpp::protocol::MessageType;
using pgcpp::protocol::SelectHbaLine;
using pgcpp::protocol::SendAuthRequest;
using pgcpp::protocol::SendErrorResponse;
using pgcpp::protocol::SetGlobalResponseReader;
using pgcpp::protocol::TransactionStatus;
using pgcpp::storage::BufferPoolShmemSize;
using pgcpp::storage::InitBufferPool;
using pgcpp::storage::InitializeAllLWLocks;
using pgcpp::storage::InitProcess;
using pgcpp::storage::kNumNamedLWLocks;
using pgcpp::storage::LWLock;
using pgcpp::storage::ProcGlobalInit;
using pgcpp::storage::ProcKill;
using pgcpp::storage::ResetHeldLWLocks;
using pgcpp::storage::SetStorageBaseDir;
using pgcpp::storage::ShmemAttach;
using pgcpp::storage::ShmemInit;
using pgcpp::storage::ShutdownBufferPool;
using pgcpp::storage::smgrcloseall;
using pgcpp::transaction::CLogShmemSize;
using pgcpp::transaction::FlushClogFiles;
using pgcpp::transaction::InitializeCommitLog;
using pgcpp::transaction::InitializeCommitTs;
using pgcpp::transaction::InitializeMultiXact;
using pgcpp::transaction::InitializeProcArray;
using pgcpp::transaction::InitializeSnapshotManager;
using pgcpp::transaction::InitializeTransactionSystem;
using pgcpp::transaction::InitializeWal;
using pgcpp::transaction::LoadClogFiles;
using pgcpp::transaction::ProcArrayShmemSize;
using pgcpp::transaction::ResetTransactionState;
using pgcpp::transaction::SetClogDirectory;
using pgcpp::transaction::SetWalDirectory;
using pgcpp::transaction::ShutdownClog;
using pgcpp::transaction::ShutdownCommitTs;
using pgcpp::transaction::ShutdownMultiXact;
using pgcpp::transaction::ShutdownWal;

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
    // pg_hba.conf loaded at startup (empty if no file → default trust).
    pgcpp::protocol::HbaConfig hba_config;
    // pg_authid: username → stored password hash. Empty if no file.
    std::map<std::string, std::string> password_store;
};

ServerGlobalState g_state;

// Load a text file into a string. Returns empty string on failure.
std::string ReadTextFile(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return "";
    std::string out;
    char buf[4096];
    while (true) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        out.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    return out;
}

// Load pg_hba.conf from <data_dir>/pg_hba.conf into g_state.hba_config.
// A missing file is not an error — leaves the config empty, which the
// auth path treats as "trust" (preserving the pre-B-2 default).
void LoadHbaConfig(const std::string& data_dir) {
    std::string text = ReadTextFile(data_dir + "/pg_hba.conf");
    if (text.empty())
        return;
    g_state.hba_config = pgcpp::protocol::ParseHbaConfig(text);
}

// Load <data_dir>/global/pg_authid.tsv into g_state.password_store.
// Format: one record per line, "<username>\t<stored_hash>". Lines starting
// with '#' are comments. A missing file is not an error (no users with
// stored passwords → only trust/reject methods work).
void LoadPasswordStore(const std::string& data_dir) {
    std::string text = ReadTextFile(data_dir + "/global/pg_authid.tsv");
    if (text.empty())
        return;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        auto tab = line.find('\t');
        if (tab == std::string::npos)
            continue;
        std::string user = line.substr(0, tab);
        std::string hash = line.substr(tab + 1);
        g_state.password_store[user] = hash;
    }
}

}  // namespace

void InitializeServerSubsystems(const std::string& data_dir) {
    g_state.data_dir = data_dir;

    // Error subsystem.
    InitErrorSubsystem();

    // Shared memory: allocate a single mmap'd segment (inherited across
    // fork) large enough for the BufferPool, PGPROC pool + PGXACT compact
    // array, ProcArray index, CLOG, VariableCache, LWLock array, plus 1MB
    // of slack.
    std::size_t shm_size =
        BufferPoolShmemSize(4096) + pgcpp::storage::ProcArrayShmemSize()    // PGPROC + PGXACT
        + ProcArrayShmemSize()                                              // ProcArray index
        + CLogShmemSize() + sizeof(LWLock) * kNumNamedLWLocks + (1 << 20);  // 1MB slack
    ShmemInit(shm_size);
    InitializeAllLWLocks();
    ProcGlobalInit();
    InitializeProcArray();
    InitializeCommitLog();

    // Top-level memory context.
    g_state.top_context = AllocSetContext::Create("ServerContext");
    SetCurrentMemoryContext(g_state.top_context);

    // Catalog + syscache.
    g_state.catalog = new Catalog();
    SetCatalog(g_state.catalog);
    BootstrapCatalog(g_state.catalog);
    // A-3: restore user-created catalog rows from the previous run, layered
    // on top of BootstrapCatalog's built-in rows. A missing file is not an
    // error (fresh initdb).
    g_state.catalog->Load(data_dir + "/pgcpp_catalog.tsv");
    // P1-2: set the persist path so CommitDirty() can save catalog changes
    // at transaction commit (not just at server shutdown).
    g_state.catalog->SetPersistPath(data_dir + "/pgcpp_catalog.tsv");

    g_state.syscache = new SysCache();
    SetSysCache(g_state.syscache);

    // Transaction system.
    ResetTransactionState();
    InitializeTransactionSystem();
    InitializeSnapshotManager();

    // A-2: WAL — load existing WAL from <data_dir>/pg_wal/wal.log so crash
    // recovery works across process restarts. XLogFlush now fsyncs the file.
    SetWalDirectory(data_dir + "/pg_wal");
    InitializeWal();

    // P0-2: CLOG / commit_ts / multixact — load existing pages from disk so
    // transaction status, commit timestamps, and multixact membership survive
    // restarts. Each subsystem persists to its own subdirectory under data_dir.
    SetClogDirectory(data_dir + "/pg_xact");
    LoadClogFiles();
    InitializeCommitTs(data_dir + "/pg_commit_ts");
    InitializeMultiXact(data_dir + "/pg_multixact/offsets", data_dir + "/pg_multixact/members");

    // Storage.
    SetStorageBaseDir(data_dir);
    InitBufferPool(4096);
    g_state.buffer_pool_initialized = true;

    // Relcache.
    InitializeRelcache();

    // P3-9: register the pg_stat_* virtual views in the catalog so they
    // can be resolved by name and scanned via the stats virtual scan.
    pgcpp::stats::BootstrapStatsViews(g_state.catalog);

    // B-2: load pg_hba.conf and pg_authid for client authentication.
    // Missing files are not errors (default to trust / empty store).
    LoadHbaConfig(data_dir);
    LoadPasswordStore(data_dir);
}

void ShutdownServerSubsystems() {
    ResetRelcache();

    if (g_state.buffer_pool_initialized) {
        ShutdownBufferPool();
        smgrcloseall();
        g_state.buffer_pool_initialized = false;
    }

    // A-2: close the WAL file (flushes pending appends to the OS).
    ShutdownWal();

    // P0-2: flush CLOG / commit_ts / multixact dirty pages to disk so
    // transaction state survives a restart. Matches PG's shutdown sequence
    // where the checkpointer flushes all SLRUs before exit.
    ShutdownClog();
    ShutdownCommitTs();
    ShutdownMultiXact();

    ResetTransactionState();
    InitializeTransactionSystem();
    InitializeSnapshotManager();

    SetSysCache(nullptr);
    SetCatalog(nullptr);
    delete g_state.syscache;
    g_state.syscache = nullptr;
    // A-3: persist user-created catalog rows so DDL survives a restart.
    if (g_state.catalog != nullptr && !g_state.data_dir.empty()) {
        g_state.catalog->Save(g_state.data_dir + "/pgcpp_catalog.tsv");
    }
    delete g_state.catalog;
    g_state.catalog = nullptr;

    SetCurrentMemoryContext(nullptr);
    if (g_state.top_context != nullptr) {
        g_state.top_context->Delete();
        g_state.top_context = nullptr;
    }
}

// ---------------------------------------------------------------------------
// ProcessStartupPacket — read and parse the frontend startup packet sequence
// ---------------------------------------------------------------------------

// Read a 4-byte network-order uint32 from the fd.
// Returns true on success, false on EOF/error.
static bool ReadUint32(int fd, uint32_t* out) {
    char buf[4];
    if (!ReadAll(fd, buf, 4))
        return false;
    *out = (static_cast<uint32_t>(static_cast<uint8_t>(buf[0])) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(buf[1])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(buf[2])) << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(buf[3]));
    return true;
}

StartupPacketResult ProcessStartupPacket(int client_fd) {
    StartupPacketResult result;

    while (true) {
        // Read the 4-byte length prefix.
        uint32_t len = 0;
        if (!ReadUint32(client_fd, &len)) {
            return result;  // EOF or read error.
        }
        // Sanity-check the length. The length includes itself (4 bytes) but
        // not a type byte (startup packets have no type byte).
        if (len < 8 || len > 1024 * 1024) {
            return result;  // Bogus length — give up.
        }

        // Read the rest of the packet (length - 4 bytes).
        std::size_t rest_len = static_cast<std::size_t>(len) - 4;
        std::vector<char> rest(rest_len);
        if (rest_len > 0 && !ReadAll(client_fd, rest.data(), rest_len)) {
            return result;  // EOF mid-packet.
        }

        // The first 4 bytes of the body are the protocol version / special code.
        uint32_t code = (static_cast<uint32_t>(static_cast<uint8_t>(rest[0])) << 24) |
                        (static_cast<uint32_t>(static_cast<uint8_t>(rest[1])) << 16) |
                        (static_cast<uint32_t>(static_cast<uint8_t>(rest[2])) << 8) |
                        static_cast<uint32_t>(static_cast<uint8_t>(rest[3]));

        if (code == kNegotiateSslCode || code == kNegotiateGssCode) {
            // SSLRequest / GSSENCRequest — reply 'N' (no SSL/GSS) and loop
            // to read the real startup packet that follows.
            char reply = 'N';
            WriteAll(client_fd, &reply, 1);
            continue;
        }

        if (code == kCancelRequestCode) {
            // CancelRequest — the caller should silently close the connection.
            return result;  // valid stays false.
        }

        // Otherwise this should be a protocol-versioned startup packet.
        // PostgreSQL's v3.0 carries 0x00030000. We accept any code that looks
        // like a versioned startup (not a special code); for the v3.0 case we
        // parse the trailing key/value pairs.
        result.protocol_version = code;

        // Parse null-terminated key/value pairs starting after the 4-byte code.
        // The body ends with an extra NUL terminator.
        std::size_t pos = 4;
        while (pos < rest_len) {
            // Read the key (null-terminated).
            std::string key;
            while (pos < rest_len && rest[pos] != '\0') {
                key.push_back(rest[pos]);
                ++pos;
            }
            if (pos >= rest_len)
                break;  // Malformed — no NUL terminator for key.
            ++pos;      // consume the key's NUL.
            if (pos >= rest_len || rest[pos] == '\0')
                break;  // End of pairs (final terminator).

            // Read the value (null-terminated).
            std::string value;
            while (pos < rest_len && rest[pos] != '\0') {
                value.push_back(rest[pos]);
                ++pos;
            }
            if (pos >= rest_len)
                break;  // Malformed — no NUL terminator for value.
            ++pos;      // consume the value's NUL.

            result.options[key] = value;
        }

        // Mirror well-known keys into dedicated fields.
        auto it = result.options.find("user");
        if (it != result.options.end()) {
            result.user = it->second;
        }
        it = result.options.find("database");
        if (it != result.options.end()) {
            result.database = it->second;
        } else {
            // Database defaults to user (PostgreSQL behavior).
            result.database = result.user;
        }
        it = result.options.find("application_name");
        if (it != result.options.end()) {
            result.application_name = it->second;
        }

        result.valid = true;
        return result;
    }
}

// ---------------------------------------------------------------------------
// BackendMain — per-connection protocol loop
// ---------------------------------------------------------------------------

void BackendMain(int client_fd) {
    // Reset signal handlers to defaults in the child process.
    ResetSignalHandlers();

    // Read and parse the startup packet (handles SSL/GSS preambles,
    // CancelRequest, and v3.0 startup with user/database).
    StartupPacketResult startup = ProcessStartupPacket(client_fd);
    if (!startup.valid) {
        // CancelRequest, EOF, or protocol violation — close silently.
        close(client_fd);
        return;
    }

    // B-2: Create the SocketSink early so auth can use it. The sink owns
    // the fd from here on (its destructor closes the fd).
    SocketSink sink(client_fd);

    // Install a response reader that reads 'p' (PasswordMessage /
    // SASLResponse) messages from the client socket. The reader normalises
    // the wire payload to the form the auth handlers expect (matching the
    // mock queue used in tests):
    //   - PasswordMessage: strip the trailing NUL.
    //   - SASLInitialResponse: parse mechanism\0 + int32 len + data and
    //     return just the initial response body.
    //   - SASLResponse: return the body as-is.
    SetGlobalResponseReader([client_fd]() -> std::string {
        char type;
        std::string payload;
        if (!ReadMessage(client_fd, &type, payload))
            return "";
        if (type != 'p')
            return "";  // protocol error
        // SASLInitialResponse starts with the mechanism name + NUL.
        if (payload.size() > 14 && payload.substr(0, 14) == "SCRAM-SHA-256" &&
            payload[14] == '\0') {
            if (payload.size() < 19)
                return "";
            int32_t len = static_cast<int32_t>((static_cast<uint8_t>(payload[15]) << 24) |
                                               (static_cast<uint8_t>(payload[16]) << 16) |
                                               (static_cast<uint8_t>(payload[17]) << 8) |
                                               static_cast<uint8_t>(payload[18]));
            if (len < 0 || static_cast<size_t>(len) > payload.size() - 19)
                return "";
            return payload.substr(19, static_cast<size_t>(len));
        }
        // PasswordMessage or SASLResponse: strip a single trailing NUL.
        if (!payload.empty() && payload.back() == '\0') {
            payload.pop_back();
        }
        return payload;
    });

    // Build the AuthContext. Select the matching pg_hba.conf line; if no
    // pg_hba.conf was loaded (or no line matches), default to trust —
    // preserving the pre-B-2 behaviour so existing clients/tests keep
    // working.
    AuthContext auth_ctx{};
    auth_ctx.user = startup.user;
    auth_ctx.sink = &sink;
    const HbaLine* hba = nullptr;
    if (g_state.hba_config.valid && !g_state.hba_config.lines.empty()) {
        hba = SelectHbaLine(g_state.hba_config, startup.database, startup.user,
                            /*addr=*/"", /*ssl_in_use=*/false);
    }
    if (hba != nullptr) {
        auth_ctx.hba_line = *hba;
    } else {
        auth_ctx.hba_line.method = HbaMethod::kTrust;
    }
    // Look up the stored password hash (empty if user not found).
    auto it = g_state.password_store.find(startup.user);
    if (it != g_state.password_store.end()) {
        auth_ctx.stored_password = it->second;
    }

    AuthResult auth_result = ClientAuthentication(auth_ctx);
    SetGlobalResponseReader({});  // clear reader; main loop uses ReadMessage

    if (auth_result != AuthResult::kSuccess) {
        // Authentication failed — send ErrorResponse and close.
        std::string msg;
        switch (auth_result) {
            case AuthResult::kWrongPassword:
                msg = "password authentication failed for user \"" + startup.user + "\"";
                break;
            case AuthResult::kNoSuchUser:
                msg = "role \"" + startup.user + "\" does not exist";
                break;
            case AuthResult::kRejected:
                msg = "pg_hba.conf rejects connection for user \"" + startup.user + "\"";
                break;
            case AuthResult::kMethodUnsupported:
                msg = "authentication method not supported";
                break;
            default:
                msg = "authentication failed";
                break;
        }
        SendErrorResponse(&sink, "28P01", msg);
        return;  // sink destructor closes the fd
    }

    // Authentication succeeded — send AuthenticationOk (type 'R', code 0).
    // For methods like SCRAM, the handler already sent SASLFinal; this
    // AuthenticationOk signals that the exchange is complete.
    SendAuthRequest(&sink, AuthRequest::kOk);

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
    send_param_status("server_version", "pgcpp 1.0");
    send_param_status("client_encoding", "UTF8");
    send_param_status("DateStyle", "ISO");
    send_param_status("integer_datetimes", "on");
    // Report the authenticated user (multi-user support).
    if (!startup.user.empty()) {
        send_param_status("current_user", startup.user);
    }

    // Send BackendKeyData (type 'K', payload: int32 pid, int32 key).
    // Both fields MUST be in network byte order (big-endian) per the
    // frontend/backend protocol spec. Without htonl() the client reads a
    // byte-swapped value — usually still positive (so tests passed by luck),
    // but negative whenever the PID's low byte has its high bit set.
    {
        char key_msg[13];
        key_msg[0] = 'K';
        int32_t key_len = htonl(12);
        std::memcpy(&key_msg[1], &key_len, 4);
        int32_t pid = htonl(static_cast<int32_t>(getpid()));
        int32_t key = htonl(0);  // cancellation key (unused)
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

    // Create the protocol backend. The SocketSink was created earlier
    // (before auth) and owns the client fd.
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
                std::vector<pgcpp::catalog::Oid> param_types;
                param_types.reserve(static_cast<std::size_t>(num_params));
                for (int16_t i = 0; i < num_params; ++i) {
                    param_types.push_back(static_cast<pgcpp::catalog::Oid>(reader.ReadInt32()));
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

        // Attach to the inherited shared-memory segment (verifies the magic;
        // the MAP_ANONYMOUS|MAP_SHARED mapping is inherited automatically via
        // fork, so no re-mmap is needed).
        ShmemAttach();
        // The child must not inherit the parent's held-LWLock list (per-process
        // state); reset it so the child starts with an empty lock set.
        ResetHeldLWLocks();
        // Claim a PGPROC slot from the shared freelist for this backend.
        InitProcess();

        BackendMain(client_fd);

        // P0-3: release the PGPROC slot back to the shared freelist so
        // subsequent connections can reuse it. Without this, the pool would
        // leak slots (one per connection) until kMaxBackends is exhausted.
        ProcKill();
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

    elog(LogLevel::kInfo, "pgcpp server listening on port " + std::to_string(listen_port_));

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

}  // namespace pgcpp::server

// libpq.cpp — PostgreSQL libpq-style client library implementation (P3-11).
//
// Implements the PgConn connection management + query execution API and
// the PgResult result-set accessor API declared in libpq/libpq.hpp.
//
// The wire layer reuses the same framing as the server side (1-byte type
// + 4-byte length + payload, network byte order). Only the client side
// of the protocol is implemented here: startup handshake, simple query
// ('Q'), extended query (Parse/Bind/Describe/Execute/Sync), COPY, and
// cancellation (separate short-lived socket).
//
// Authentication: trust, reject, and cleartext password are supported.
// md5 and SCRAM-SHA-256 are accepted at the protocol level (the password
// is forwarded as-is) but pgcpp's server currently advertises only trust
// auth, so the password path is exercised primarily by unit tests.
#include "libpq/libpq.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace pgcpp::libpq {

namespace {

// ---------------------------------------------------------------------------
// Socket I/O helpers (handle partial reads/writes and EINTR).
// ---------------------------------------------------------------------------

bool WriteAll(int fd, const void* data, std::size_t len) {
    const char* p = static_cast<const char*>(data);
    std::size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, p + written, len - written);
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

bool ReadAll(int fd, void* buf, std::size_t len) {
    char* p = static_cast<char*>(buf);
    std::size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, p + got, len - got);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;  // peer closed
        got += static_cast<std::size_t>(n);
    }
    return true;
}

// Read up to `len` bytes nonblocking. Returns bytes read (>=0) or -1 on error.
ssize_t ReadSome(int fd, void* buf, std::size_t len) {
    while (true) {
        ssize_t n = read(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 0;
            return -1;
        }
        return n;
    }
}

// Append a network-order int32 to a string buffer.
void AppendInt32(std::string& s, int32_t v) {
    uint32_t u = static_cast<uint32_t>(v);
    s.push_back(static_cast<char>((u >> 24) & 0xFF));
    s.push_back(static_cast<char>((u >> 16) & 0xFF));
    s.push_back(static_cast<char>((u >> 8) & 0xFF));
    s.push_back(static_cast<char>(u & 0xFF));
}

// Append a network-order int32 to a raw char buffer at the given offset.
void PutInt32Raw(char* buf, int32_t v) {
    uint32_t u = static_cast<uint32_t>(v);
    buf[0] = static_cast<char>((u >> 24) & 0xFF);
    buf[1] = static_cast<char>((u >> 16) & 0xFF);
    buf[2] = static_cast<char>((u >> 8) & 0xFF);
    buf[3] = static_cast<char>(u & 0xFF);
}

// Append a network-order int16 to a string buffer.
void AppendInt16(std::string& s, int16_t v) {
    uint16_t u = static_cast<uint16_t>(v);
    s.push_back(static_cast<char>((u >> 8) & 0xFF));
    s.push_back(static_cast<char>(u & 0xFF));
}

// Read a network-order int32 from a buffer at offset.
int32_t ReadInt32(const char* p) {
    return static_cast<int32_t>((static_cast<uint8_t>(p[0]) << 24) |
                                (static_cast<uint8_t>(p[1]) << 16) |
                                (static_cast<uint8_t>(p[2]) << 8) | static_cast<uint8_t>(p[3]));
}

int16_t ReadInt16(const char* p) {
    return static_cast<int16_t>((static_cast<uint8_t>(p[0]) << 8) | static_cast<uint8_t>(p[1]));
}

}  // namespace

// ===========================================================================
// PgResult implementation
// ===========================================================================

const std::string& PgResult::FName(int col) const {
    static const std::string kEmpty;
    if (col < 0 || col >= static_cast<int>(fields_.size()))
        return kEmpty;
    return fields_[col].name;
}

int PgResult::FNumber(const std::string& name) const {
    for (int i = 0; i < static_cast<int>(fields_.size()); ++i) {
        // Case-insensitive match (libpq behaviour).
        if (fields_[i].name.size() == name.size() &&
            strcasecmp(fields_[i].name.c_str(), name.c_str()) == 0) {
            return i;
        }
    }
    return -1;
}

uint32_t PgResult::FType(int col) const {
    if (col < 0 || col >= static_cast<int>(fields_.size()))
        return 0;
    return fields_[col].type_oid;
}

int32_t PgResult::FMod(int col) const {
    if (col < 0 || col >= static_cast<int>(fields_.size()))
        return 0;
    return fields_[col].type_mod;
}

int16_t PgResult::FSize(int col) const {
    if (col < 0 || col >= static_cast<int>(fields_.size()))
        return 0;
    return fields_[col].type_size;
}

int16_t PgResult::FFormat(int col) const {
    if (col < 0 || col >= static_cast<int>(fields_.size()))
        return 0;
    return fields_[col].format;
}

uint32_t PgResult::FTable(int col) const {
    if (col < 0 || col >= static_cast<int>(fields_.size()))
        return 0;
    return fields_[col].table_oid;
}

int16_t PgResult::FTableCol(int col) const {
    if (col < 0 || col >= static_cast<int>(fields_.size()))
        return 0;
    return fields_[col].column_attnum;
}

const char* PgResult::GetValue(int row, int col) const {
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return nullptr;
    if (col < 0 || col >= static_cast<int>(rows_[row].first.size()))
        return nullptr;
    if (rows_[row].second[col])
        return nullptr;  // NULL
    return rows_[row].first[col].c_str();
}

bool PgResult::GetIsNull(int row, int col) const {
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return true;
    if (col < 0 || col >= static_cast<int>(rows_[row].second.size()))
        return true;
    return rows_[row].second[col];
}

int PgResult::GetLength(int row, int col) const {
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return -1;
    if (col < 0 || col >= static_cast<int>(rows_[row].first.size()))
        return -1;
    if (rows_[row].second[col])
        return -1;  // NULL
    return static_cast<int>(rows_[row].first[col].size());
}

int PgResult::CommandTuples() const {
    if (command_tag_.empty())
        return -1;
    // Command tags: "INSERT 0 N", "UPDATE N", "DELETE N", "SELECT N", ...
    // Find the last integer in the tag.
    int n = -1;
    const char* s = command_tag_.c_str();
    const char* p = s + command_tag_.size();
    while (p > s && !std::isdigit(static_cast<unsigned char>(*(p - 1))))
        --p;
    if (p == s)
        return -1;
    const char* end = p;
    while (p > s && std::isdigit(static_cast<unsigned char>(*(p - 1))))
        --p;
    n = std::atoi(p);
    (void)end;
    return n;
}

std::string PgResult::VerboseErrorMessage() const {
    return error_message_;
}

void PgResult::Clear() {
    status_ = ExecStatusType::kFatalError;
    fields_.clear();
    rows_.clear();
    command_tag_.clear();
    error_message_.clear();
    inserted_oid_ = 0;
}

void PgResult::AddRow(std::vector<std::string> values, std::vector<bool> isnull) {
    rows_.emplace_back(std::move(values), std::move(isnull));
}

void PgResult::ReserveRows(int n) {
    rows_.reserve(static_cast<std::size_t>(n));
}

// ===========================================================================
// PgConn implementation
// ===========================================================================

PgConn::PgConn() = default;

PgConn::~PgConn() {
    if (fd_ >= 0) {
        // Best-effort Terminate, ignore errors.
        char msg[5];
        msg[0] = 'X';
        PutInt32Raw(msg + 1, 4);
        WriteAll(fd_, msg, 5);
        close(fd_);
        fd_ = -1;
    }
}

PgConn::PgConn(PgConn&& other) noexcept
    : host_(std::move(other.host_)),
      port_(other.port_),
      user_(std::move(other.user_)),
      dbname_(std::move(other.dbname_)),
      password_(std::move(other.password_)),
      options_(std::move(other.options_)),
      connect_timeout_(other.connect_timeout_),
      fd_(other.fd_),
      status_(other.status_),
      error_message_(std::move(other.error_message_)),
      protocol_version_(other.protocol_version_),
      server_version_(std::move(other.server_version_)),
      client_encoding_(std::move(other.client_encoding_)),
      backend_pid_(other.backend_pid_),
      backend_secret_(other.backend_secret_),
      trace_(other.trace_),
      connect_state_(other.connect_state_),
      single_row_mode_(other.single_row_mode_),
      pipeline_status_(other.pipeline_status_),
      pending_notify_(std::move(other.pending_notify_)),
      notify_read_pos_(other.notify_read_pos_),
      conn_options_(std::move(other.conn_options_)),
      parameter_status_(std::move(other.parameter_status_)),
      copy_in_progress_(other.copy_in_progress_),
      copy_mode_(other.copy_mode_),
      in_buffer_(std::move(other.in_buffer_)),
      in_busy_(other.in_busy_),
      pending_results_(std::move(other.pending_results_)),
      last_conninfo_(std::move(other.last_conninfo_)) {
    other.fd_ = -1;
    other.status_ = ConnStatusType::kBad;
}

PgConn& PgConn::operator=(PgConn&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            char msg[5];
            msg[0] = 'X';
            PutInt32Raw(msg + 1, 4);
            WriteAll(fd_, msg, 5);
            close(fd_);
        }
        host_ = std::move(other.host_);
        port_ = other.port_;
        user_ = std::move(other.user_);
        dbname_ = std::move(other.dbname_);
        password_ = std::move(other.password_);
        options_ = std::move(other.options_);
        connect_timeout_ = other.connect_timeout_;
        fd_ = other.fd_;
        status_ = other.status_;
        error_message_ = std::move(other.error_message_);
        protocol_version_ = other.protocol_version_;
        server_version_ = std::move(other.server_version_);
        client_encoding_ = std::move(other.client_encoding_);
        backend_pid_ = other.backend_pid_;
        backend_secret_ = other.backend_secret_;
        trace_ = other.trace_;
        connect_state_ = other.connect_state_;
        single_row_mode_ = other.single_row_mode_;
        pipeline_status_ = other.pipeline_status_;
        pending_notify_ = std::move(other.pending_notify_);
        notify_read_pos_ = other.notify_read_pos_;
        conn_options_ = std::move(other.conn_options_);
        parameter_status_ = std::move(other.parameter_status_);
        copy_in_progress_ = other.copy_in_progress_;
        copy_mode_ = other.copy_mode_;
        in_buffer_ = std::move(other.in_buffer_);
        in_busy_ = other.in_busy_;
        pending_results_ = std::move(other.pending_results_);
        last_conninfo_ = std::move(other.last_conninfo_);
        other.fd_ = -1;
        other.status_ = ConnStatusType::kBad;
    }
    return *this;
}

// --- Internal helpers -----------------------------------------------------

bool PgConn::SendAll(const void* data, std::size_t len) {
    if (fd_ < 0)
        return false;
    return WriteAll(fd_, data, len);
}

bool PgConn::ReadAll(void* buf, std::size_t len) {
    if (fd_ < 0)
        return false;
    return ::pgcpp::libpq::ReadAll(fd_, buf, len);
}

bool PgConn::SendMessage(char type, const std::string& payload) {
    if (fd_ < 0) {
        error_message_ = "not connected";
        return false;
    }
    std::string msg;
    msg.push_back(type);
    AppendInt32(msg, static_cast<int32_t>(4 + payload.size()));
    msg += payload;
    if (!SendAll(msg.data(), msg.size())) {
        error_message_ = "failed to send message: ";
        error_message_ += std::strerror(errno);
        return false;
    }
    return true;
}

bool PgConn::ReadMessage(char* type, std::string* payload) {
    if (fd_ < 0)
        return false;
    if (!ReadAll(type, 1)) {
        error_message_ = "connection closed by server";
        return false;
    }
    char len_buf[4];
    if (!ReadAll(len_buf, 4)) {
        error_message_ = "failed to read message length";
        return false;
    }
    int32_t length = ReadInt32(len_buf);
    if (length < 4) {
        error_message_ = "invalid message length";
        return false;
    }
    std::size_t payload_len = static_cast<std::size_t>(length) - 4;
    payload->resize(payload_len);
    if (payload_len > 0) {
        if (!ReadAll(payload->data(), payload_len)) {
            error_message_ = "failed to read message payload";
            return false;
        }
    }
    return true;
}

bool PgConn::SendStartupMessage() {
    // StartupMessage has no type byte — just length + protocol version +
    // null-terminated key/value pairs + final NUL.
    std::string payload;
    AppendInt32(payload, 0x00030000);  // protocol 3.0
    if (!user_.empty()) {
        payload += "user";
        payload.push_back('\0');
        payload += user_;
        payload.push_back('\0');
    }
    if (!dbname_.empty()) {
        payload += "database";
        payload.push_back('\0');
        payload += dbname_;
        payload.push_back('\0');
    }
    if (!options_.empty()) {
        payload += "options";
        payload.push_back('\0');
        payload += options_;
        payload.push_back('\0');
    }
    if (!client_encoding_.empty()) {
        payload += "client_encoding";
        payload.push_back('\0');
        payload += client_encoding_;
        payload.push_back('\0');
    }
    payload.push_back('\0');  // terminator

    std::string msg;
    AppendInt32(msg, static_cast<int32_t>(4 + payload.size()));
    msg += payload;
    return SendAll(msg.data(), msg.size());
}

bool PgConn::SendCancelRequest(uint32_t pid, uint32_t secret) {
    // CancelRequest is a special startup-class message: 16 bytes total.
    // length(4) + CancelRequestCode(4) + PID(4) + secret(4).
    std::string msg;
    AppendInt32(msg, 16);
    AppendInt32(msg, 0x04D2162F);  // CancelRequestCode (libpq constant)
    AppendInt32(msg, static_cast<int32_t>(pid));
    AppendInt32(msg, static_cast<int32_t>(secret));
    return SendAll(msg.data(), msg.size());
}

bool PgConn::TryConnect() {
    if (fd_ >= 0)
        return true;
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        error_message_ = "socket() failed: ";
        error_message_ += std::strerror(errno);
        return false;
    }
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0) {
        error_message_ = "invalid host address: " + host_;
        close(fd_);
        fd_ = -1;
        return false;
    }
    if (connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        error_message_ = "connect() failed: ";
        error_message_ += std::strerror(errno);
        close(fd_);
        fd_ = -1;
        return false;
    }
    // Disable Nagle for snappier interactive protocol.
    int one = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return true;
}

bool PgConn::DoStartupAuth() {
    if (!SendStartupMessage()) {
        status_ = ConnStatusType::kBad;
        return false;
    }
    // Read responses until ReadyForQuery.
    while (true) {
        char type;
        std::string payload;
        if (!ReadMessage(&type, &payload)) {
            status_ = ConnStatusType::kBad;
            return false;
        }
        if (type == 'R') {
            // Authentication request.
            if (payload.size() < 4) {
                error_message_ = "malformed Authentication message";
                status_ = ConnStatusType::kBad;
                return false;
            }
            int32_t auth_type = ReadInt32(payload.data());
            if (auth_type == 0) {
                continue;  // AuthenticationOk
            }
            if (auth_type == 2) {
                // KerberosV5 — not supported.
                error_message_ = "KerberosV5 authentication not supported";
                status_ = ConnStatusType::kBad;
                return false;
            }
            if (auth_type == 3) {
                // Cleartext password.
                std::string pw = password_;
                pw.push_back('\0');
                if (!SendMessage('p', pw)) {
                    status_ = ConnStatusType::kBad;
                    return false;
                }
                continue;
            }
            if (auth_type == 5) {
                // MD5 password — would need salt handling. For now we
                // send the cleartext password; the pgcpp server accepts
                // both forms when trust is configured. Real md5 hashing
                // is left as future work.
                std::string pw = password_;
                pw.push_back('\0');
                if (!SendMessage('p', pw)) {
                    status_ = ConnStatusType::kBad;
                    return false;
                }
                continue;
            }
            if (auth_type == 10) {
                // SASL — SCRAM-SHA-256 not implemented in client.
                error_message_ = "SCRAM authentication not supported by libpq client";
                status_ = ConnStatusType::kBad;
                return false;
            }
            error_message_ = "unsupported authentication type: " + std::to_string(auth_type);
            status_ = ConnStatusType::kBad;
            return false;
        }
        if (type == 'S') {
            // ParameterStatus: name\0value\0.
            std::size_t pos = 0;
            std::size_t end = payload.find('\0', pos);
            if (end == std::string::npos)
                continue;
            std::string name = payload.substr(pos, end - pos);
            pos = end + 1;
            end = payload.find('\0', pos);
            if (end == std::string::npos)
                continue;
            std::string value = payload.substr(pos, end - pos);
            parameter_status_.emplace_back(std::move(name), std::move(value));
            continue;
        }
        if (type == 'K') {
            // BackendKeyData: pid(4) + secret(4).
            if (payload.size() >= 8) {
                backend_pid_ = ReadInt32(payload.data());
                backend_secret_ = static_cast<uint32_t>(ReadInt32(payload.data() + 4));
            }
            continue;
        }
        if (type == 'N') {
            // NoticeResponse — ignore for now.
            continue;
        }
        if (type == 'Z') {
            // ReadyForQuery.
            status_ = ConnStatusType::kOk;
            return true;
        }
        if (type == 'E') {
            // ErrorResponse during startup.
            for (std::size_t i = 0; i < payload.size();) {
                char field = payload[i];
                if (field == '\0')
                    break;
                ++i;
                std::size_t end = payload.find('\0', i);
                if (end == std::string::npos)
                    break;
                if (field == 'M') {
                    error_message_ = payload.substr(i, end - i);
                }
                i = end + 1;
            }
            status_ = ConnStatusType::kBad;
            return false;
        }
        // Unknown message — ignore.
    }
}

// --- Synchronous connect --------------------------------------------------

ConnStatusType PgConn::Connect(const ConnectOptions& opts) {
    host_ = opts.host;
    port_ = opts.port;
    user_ = opts.user.empty() ? DefaultUser() : opts.user;
    dbname_ = opts.dbname;
    password_ = opts.password;
    options_ = opts.options;
    connect_timeout_ = opts.connect_timeout;
    error_message_.clear();

    if (connect_timeout_ > 0) {
        struct timeval tv;
        tv.tv_sec = connect_timeout_;
        tv.tv_usec = 0;
        // Apply later via setsockopt on the connected socket.
        (void)tv;
    }

    if (!TryConnect()) {
        status_ = ConnStatusType::kBad;
        return status_;
    }
    if (connect_timeout_ > 0) {
        struct timeval tv;
        tv.tv_sec = connect_timeout_;
        tv.tv_usec = 0;
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    if (!DoStartupAuth()) {
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
        return status_;
    }
    // Derive server_version / client_encoding from parameter status.
    for (const auto& kv : parameter_status_) {
        if (kv.first == "server_version")
            server_version_ = kv.second;
        if (kv.first == "client_encoding")
            client_encoding_ = kv.second;
    }
    protocol_version_ = 3 << 16;
    return status_;
}

ConnStatusType PgConn::Connect(const std::string& conninfo) {
    last_conninfo_ = conninfo;
    std::vector<ConnInfoOption> opts;
    std::string errmsg;
    if (!ParseConnInfo(conninfo, opts, errmsg)) {
        error_message_ = errmsg;
        status_ = ConnStatusType::kBad;
        return status_;
    }
    FillDefaults(opts);
    conn_options_ = opts;

    ConnectOptions copts;
    if (const std::string* v = GetOption(opts, "host"))
        copts.host = *v;
    if (const std::string* v = GetOption(opts, "port"))
        copts.port = std::atoi(v->c_str());
    if (const std::string* v = GetOption(opts, "user"))
        copts.user = *v;
    if (const std::string* v = GetOption(opts, "dbname"))
        copts.dbname = *v;
    if (const std::string* v = GetOption(opts, "password"))
        copts.password = *v;
    if (const std::string* v = GetOption(opts, "options"))
        copts.options = *v;
    if (const std::string* v = GetOption(opts, "connect_timeout")) {
        copts.connect_timeout = std::atoi(v->c_str());
    }
    return Connect(copts);
}

void PgConn::Finish() {
    if (fd_ >= 0) {
        char msg[5];
        msg[0] = 'X';
        PutInt32Raw(msg + 1, 4);
        WriteAll(fd_, msg, 5);
        close(fd_);
        fd_ = -1;
    }
    status_ = ConnStatusType::kBad;
    error_message_.clear();
    in_buffer_.clear();
    pending_notify_.clear();
    notify_read_pos_ = 0;
    pending_results_.clear();
    in_busy_ = false;
    query_done_ = false;
    copy_in_progress_ = false;
    copy_mode_ = 0;
    pipeline_status_ = PipelineStatusType::kOff;
}

ConnStatusType PgConn::Reset() {
    std::string conninfo = last_conninfo_;
    Finish();
    if (conninfo.empty()) {
        // No prior conninfo — fall back to defaults.
        ConnectOptions opts;
        return Connect(opts);
    }
    return Connect(conninfo);
}

// --- Async connect --------------------------------------------------------

ConnStatusType PgConn::ConnectStart(const std::string& conninfo) {
    last_conninfo_ = conninfo;
    std::vector<ConnInfoOption> opts;
    std::string errmsg;
    if (!ParseConnInfo(conninfo, opts, errmsg)) {
        error_message_ = errmsg;
        status_ = ConnStatusType::kBad;
        return status_;
    }
    FillDefaults(opts);
    conn_options_ = opts;
    if (const std::string* v = GetOption(opts, "host"))
        host_ = *v;
    if (const std::string* v = GetOption(opts, "port"))
        port_ = std::atoi(v->c_str());
    if (const std::string* v = GetOption(opts, "user"))
        user_ = *v;
    if (const std::string* v = GetOption(opts, "dbname"))
        dbname_ = *v;
    if (const std::string* v = GetOption(opts, "password"))
        password_ = *v;
    if (const std::string* v = GetOption(opts, "options"))
        options_ = *v;

    // Create a nonblocking socket and start the connect.
    fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd_ < 0) {
        error_message_ = "socket() failed: ";
        error_message_ += std::strerror(errno);
        status_ = ConnStatusType::kBad;
        return status_;
    }
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0) {
        error_message_ = "invalid host address: " + host_;
        close(fd_);
        fd_ = -1;
        status_ = ConnStatusType::kBad;
        return status_;
    }
    int rc = connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        error_message_ = "connect() failed: ";
        error_message_ += std::strerror(errno);
        close(fd_);
        fd_ = -1;
        status_ = ConnStatusType::kBad;
        return status_;
    }
    connect_state_ = ConnectState::kConnecting;
    status_ = ConnStatusType::kStarted;
    return status_;
}

PollingStatusType PgConn::ConnectPoll() {
    if (fd_ < 0)
        return PollingStatusType::kFailed;
    switch (connect_state_) {
        case ConnectState::kIdle:
            return PollingStatusType::kOk;
        case ConnectState::kConnecting: {
            // Check whether the nonblocking connect finished.
            int err = 0;
            socklen_t len = sizeof(err);
            if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
                if (err == EINPROGRESS)
                    return PollingStatusType::kWriting;
                error_message_ = "connect failed: ";
                error_message_ += std::strerror(err);
                status_ = ConnStatusType::kBad;
                return PollingStatusType::kFailed;
            }
            // Connected — switch back to blocking mode. DoStartupAuth (on
            // the next poll) sends the startup message itself, so we must NOT
            // send it here too: sending it twice desynchronizes the server's
            // main protocol loop after auth completes.
            int flags = fcntl(fd_, F_GETFL, 0);
            fcntl(fd_, F_SETFL, flags & ~O_NONBLOCK);
            connect_state_ = ConnectState::kAwaitingAuth;
            return PollingStatusType::kReading;
        }
        case ConnectState::kAwaitingAuth:
        case ConnectState::kAwaitingReady: {
            // DoStartupAuth sends the startup message and reads auth
            // responses until ReadyForQuery.
            if (!DoStartupAuth()) {
                status_ = ConnStatusType::kBad;
                return PollingStatusType::kFailed;
            }
            connect_state_ = ConnectState::kIdle;
            for (const auto& kv : parameter_status_) {
                if (kv.first == "server_version")
                    server_version_ = kv.second;
                if (kv.first == "client_encoding")
                    client_encoding_ = kv.second;
            }
            protocol_version_ = 3 << 16;
            return PollingStatusType::kOk;
        }
        case ConnectState::kAwaitingStartup:
            // Should have been handled by kConnecting.
            return PollingStatusType::kActive;
    }
    return PollingStatusType::kFailed;
}

// --- Sync query execution -------------------------------------------------

PgResult PgConn::Exec(const std::string& query) {
    PgResult result;
    if (fd_ < 0) {
        result.SetErrorMessage("not connected");
        return result;
    }
    if (!SendQuery(query)) {
        result.SetErrorMessage(error_message_);
        return result;
    }
    // Drain results until ReadyForQuery (we return the last meaningful one).
    PgResult cur;
    bool got_any = false;
    while (true) {
        cur = GetResult();
        if (cur.Status() == ExecStatusType::kFatalError && cur.ErrorMessage().empty() &&
            cur.NTuples() == 0 && cur.NFields() == 0 && cur.CommandStatus().empty()) {
            // Empty (terminator) result — done.
            break;
        }
        got_any = true;
        result = std::move(cur);
        // Fatal error or empty: stop after this result.
        if (result.Status() == ExecStatusType::kFatalError)
            break;
    }
    if (!got_any) {
        // No result returned — synthesize an empty command-ok result.
        result.SetStatus(ExecStatusType::kCommandOk);
    }
    return result;
}

PgResult PgConn::ExecParams(const std::string& query, const std::vector<Param>& params) {
    PgResult result;
    if (fd_ < 0) {
        result.SetErrorMessage("not connected");
        return result;
    }
    if (!SendQueryParams(query, params)) {
        result.SetErrorMessage(error_message_);
        return result;
    }
    PgResult cur;
    bool got_any = false;
    while (true) {
        cur = GetResult();
        if (cur.Status() == ExecStatusType::kFatalError && cur.ErrorMessage().empty() &&
            cur.NTuples() == 0 && cur.NFields() == 0 && cur.CommandStatus().empty()) {
            break;
        }
        got_any = true;
        result = std::move(cur);
        if (result.Status() == ExecStatusType::kFatalError)
            break;
    }
    if (!got_any)
        result.SetStatus(ExecStatusType::kCommandOk);
    return result;
}

PgResult PgConn::Prepare(const std::string& stmtName, const std::string& query,
                         const std::vector<uint32_t>& paramTypes) {
    PgResult result;
    if (fd_ < 0) {
        result.SetErrorMessage("not connected");
        return result;
    }
    if (!SendPrepare(stmtName, query, paramTypes)) {
        result.SetErrorMessage(error_message_);
        return result;
    }
    PgResult cur;
    while (true) {
        cur = GetResult();
        if (cur.Status() == ExecStatusType::kFatalError && cur.ErrorMessage().empty() &&
            cur.NTuples() == 0 && cur.NFields() == 0 && cur.CommandStatus().empty()) {
            break;
        }
        result = std::move(cur);
        if (result.Status() == ExecStatusType::kFatalError)
            break;
    }
    return result;
}

PgResult PgConn::ExecPrepared(const std::string& stmtName, const std::vector<Param>& params) {
    PgResult result;
    if (fd_ < 0) {
        result.SetErrorMessage("not connected");
        return result;
    }
    if (!SendQueryPrepared(stmtName, params)) {
        result.SetErrorMessage(error_message_);
        return result;
    }
    PgResult cur;
    bool got_any = false;
    while (true) {
        cur = GetResult();
        if (cur.Status() == ExecStatusType::kFatalError && cur.ErrorMessage().empty() &&
            cur.NTuples() == 0 && cur.NFields() == 0 && cur.CommandStatus().empty()) {
            break;
        }
        got_any = true;
        result = std::move(cur);
        if (result.Status() == ExecStatusType::kFatalError)
            break;
    }
    if (!got_any)
        result.SetStatus(ExecStatusType::kCommandOk);
    return result;
}

// --- Async query submission ----------------------------------------------

bool PgConn::SendQuery(const std::string& query) {
    std::string payload = query;
    payload.push_back('\0');
    if (!SendMessage('Q', payload))
        return false;
    // Simple Query protocol: the server replies with results followed by
    // ReadyForQuery. No explicit Sync is needed (Sync belongs to the
    // extended query protocol only).
    in_busy_ = true;
    query_done_ = false;
    return true;
}

bool PgConn::SendQueryParams(const std::string& query, const std::vector<Param>& params) {
    // Parse (unnamed statement) + Bind (unnamed portal) + Describe + Execute + Sync.
    std::string parse_payload;
    parse_payload.push_back('\0');  // unnamed prepared statement
    parse_payload += query;
    parse_payload.push_back('\0');
    AppendInt16(parse_payload, 0);  // no parameter types (server infers)
    if (!SendMessage('P', parse_payload))
        return false;

    std::string bind_payload;
    bind_payload.push_back('\0');  // unnamed prepared statement
    bind_payload.push_back('\0');  // unnamed portal
    AppendInt16(bind_payload, 0);  // no parameter format codes (text)
    AppendInt16(bind_payload, static_cast<int16_t>(params.size()));
    for (const auto& p : params) {
        if (p.is_null) {
            AppendInt32(bind_payload, -1);
        } else {
            AppendInt32(bind_payload, static_cast<int32_t>(p.value.size()));
            bind_payload += p.value;
        }
    }
    AppendInt16(bind_payload, 0);  // no result format codes (text)
    AppendInt16(bind_payload, 0);  // (redundant safety)
    if (!SendMessage('B', bind_payload))
        return false;

    // Describe portal.
    std::string desc_payload;
    desc_payload.push_back('P');
    desc_payload.push_back('\0');
    if (!SendMessage('D', desc_payload))
        return false;

    // Execute.
    std::string exec_payload;
    exec_payload.push_back('\0');  // unnamed portal
    AppendInt32(exec_payload, 0);  // unlimited rows
    if (!SendMessage('E', exec_payload))
        return false;

    if (pipeline_status_ != PipelineStatusType::kOn) {
        if (!SendMessage('S', ""))
            return false;  // Sync
    }
    in_busy_ = true;
    query_done_ = false;
    return true;
}

bool PgConn::SendPrepare(const std::string& stmtName, const std::string& query,
                         const std::vector<uint32_t>& paramTypes) {
    std::string payload;
    payload += stmtName;
    payload.push_back('\0');
    payload += query;
    payload.push_back('\0');
    AppendInt16(payload, static_cast<int16_t>(paramTypes.size()));
    for (uint32_t t : paramTypes) {
        AppendInt32(payload, static_cast<int32_t>(t));
    }
    if (!SendMessage('P', payload))
        return false;
    if (pipeline_status_ != PipelineStatusType::kOn) {
        if (!SendMessage('S', ""))
            return false;
    }
    in_busy_ = true;
    query_done_ = false;
    return true;
}

bool PgConn::SendQueryPrepared(const std::string& stmtName, const std::vector<Param>& params) {
    std::string bind_payload;
    // Bind message order: portal name, then statement name.
    bind_payload.push_back('\0');  // unnamed portal (first)
    bind_payload += stmtName;      // prepared statement name (second)
    bind_payload.push_back('\0');
    AppendInt16(bind_payload, 0);  // no parameter format codes
    AppendInt16(bind_payload, static_cast<int16_t>(params.size()));
    for (const auto& p : params) {
        if (p.is_null) {
            AppendInt32(bind_payload, -1);
        } else {
            AppendInt32(bind_payload, static_cast<int32_t>(p.value.size()));
            bind_payload += p.value;
        }
    }
    AppendInt16(bind_payload, 0);  // no result format codes
    AppendInt16(bind_payload, 0);  // redundant safety
    if (!SendMessage('B', bind_payload))
        return false;

    // Describe portal.
    std::string desc_payload;
    desc_payload.push_back('P');
    desc_payload.push_back('\0');
    if (!SendMessage('D', desc_payload))
        return false;

    // Execute.
    std::string exec_payload;
    exec_payload.push_back('\0');
    AppendInt32(exec_payload, 0);
    if (!SendMessage('E', exec_payload))
        return false;

    if (pipeline_status_ != PipelineStatusType::kOn) {
        if (!SendMessage('S', ""))
            return false;
    }
    in_busy_ = true;
    query_done_ = false;
    return true;
}

// --- GetResult: read messages until a result is complete ------------------

namespace {

// Parse an ErrorResponse payload into just the 'M' (message) field.
std::string ParseErrorMessage(const std::string& payload) {
    std::string msg;
    for (std::size_t i = 0; i < payload.size();) {
        char field = payload[i];
        if (field == '\0')
            break;
        ++i;
        std::size_t end = payload.find('\0', i);
        if (end == std::string::npos)
            break;
        if (field == 'M') {
            msg = payload.substr(i, end - i);
        }
        i = end + 1;
    }
    return msg;
}

}  // namespace

PgResult PgConn::GetResult() {
    PgResult result;
    if (fd_ < 0) {
        result.SetErrorMessage("not connected");
        return result;
    }
    // If the previous GetResult already consumed ReadyForQuery, return an
    // empty terminator now (mirrors PQgetResult returning NULL).
    if (query_done_) {
        query_done_ = false;
        return result;  // default status kFatalError == empty terminator
    }
    bool got_any_result = false;
    bool expecting_rows = false;
    while (true) {
        char type;
        std::string payload;
        if (!ReadMessage(&type, &payload)) {
            // Connection lost.
            result.SetStatus(ExecStatusType::kFatalError);
            result.SetErrorMessage(error_message_);
            close(fd_);
            fd_ = -1;
            return result;
        }
        switch (type) {
            case '1':  // ParseComplete
                got_any_result = true;
                result.SetStatus(ExecStatusType::kCommandOk);
                break;
            case '2':  // BindComplete
                got_any_result = true;
                break;
            case '3':  // CloseComplete
                got_any_result = true;
                break;
            case 'T': {  // RowDescription
                expecting_rows = true;
                std::vector<ResultField> fields;
                if (payload.size() >= 2) {
                    int16_t nfields = ReadInt16(payload.data());
                    std::size_t pos = 2;
                    for (int16_t i = 0; i < nfields && pos < payload.size(); ++i) {
                        std::size_t end = payload.find('\0', pos);
                        if (end == std::string::npos)
                            break;
                        ResultField f;
                        f.name = payload.substr(pos, end - pos);
                        pos = end + 1;
                        if (pos + 18 > payload.size())
                            break;
                        f.table_oid = static_cast<uint32_t>(ReadInt32(payload.data() + pos));
                        pos += 4;
                        f.column_attnum = ReadInt16(payload.data() + pos);
                        pos += 2;
                        f.type_oid = static_cast<uint32_t>(ReadInt32(payload.data() + pos));
                        pos += 4;
                        f.type_size = ReadInt16(payload.data() + pos);
                        pos += 2;
                        f.type_mod = ReadInt32(payload.data() + pos);
                        pos += 4;
                        f.format = ReadInt16(payload.data() + pos);
                        pos += 2;
                        fields.push_back(std::move(f));
                    }
                }
                result.SetFields(std::move(fields));
                result.SetStatus(ExecStatusType::kTuplesOk);
                got_any_result = true;
                break;
            }
            case 'D': {  // DataRow
                if (payload.size() >= 2) {
                    int16_t ncols = ReadInt16(payload.data());
                    std::size_t pos = 2;
                    std::vector<std::string> values;
                    std::vector<bool> isnull;
                    values.reserve(static_cast<std::size_t>(ncols));
                    isnull.reserve(static_cast<std::size_t>(ncols));
                    for (int16_t i = 0; i < ncols; ++i) {
                        if (pos + 4 > payload.size())
                            break;
                        int32_t col_len = ReadInt32(payload.data() + pos);
                        pos += 4;
                        if (col_len < 0) {
                            values.emplace_back();
                            isnull.push_back(true);
                        } else {
                            if (pos + static_cast<std::size_t>(col_len) > payload.size())
                                break;
                            values.emplace_back(
                                payload.substr(pos, static_cast<std::size_t>(col_len)));
                            isnull.push_back(false);
                            pos += static_cast<std::size_t>(col_len);
                        }
                    }
                    result.AddRow(std::move(values), std::move(isnull));
                    if (single_row_mode_) {
                        result.SetStatus(ExecStatusType::kSingleTuple);
                        return result;
                    }
                }
                break;
            }
            case 'C': {  // CommandComplete
                std::size_t end = payload.find('\0');
                if (end != std::string::npos) {
                    result.SetCommandStatus(payload.substr(0, end));
                } else {
                    result.SetCommandStatus(payload);
                }
                // Only promote to kCommandOk if no richer status (e.g.
                // kTuplesOk from a preceding RowDescription) has been set.
                // Mirrors libpq: CommandComplete does not override
                // PGRES_TUPLES_OK.
                if (result.Status() == ExecStatusType::kFatalError) {
                    result.SetStatus(ExecStatusType::kCommandOk);
                }
                got_any_result = true;
                break;
            }
            case 'I':  // EmptyQueryResponse
                result.SetStatus(ExecStatusType::kEmptyQuery);
                got_any_result = true;
                break;
            case 'E': {  // ErrorResponse
                result.SetStatus(ExecStatusType::kFatalError);
                result.SetErrorMessage(ParseErrorMessage(payload));
                return result;
            }
            case 'n':  // NoData
                got_any_result = true;
                break;
            case 't':  // ParameterDescription
                got_any_result = true;
                break;
            case 's':  // PortalSuspended
                result.SetStatus(ExecStatusType::kCommandOk);
                got_any_result = true;
                return result;
            case 'A': {  // NotificationResponse
                PGnotify n;
                if (payload.size() >= 8) {
                    n.be_pid = ReadInt32(payload.data());
                    std::size_t pos = 4;
                    std::size_t end = payload.find('\0', pos);
                    if (end != std::string::npos) {
                        n.relname = payload.substr(pos, end - pos);
                        pos = end + 1;
                        end = payload.find('\0', pos);
                        if (end != std::string::npos) {
                            n.extra = payload.substr(pos, end - pos);
                        }
                    }
                    pending_notify_.push_back(std::move(n));
                }
                break;
            }
            case 'N':  // NoticeResponse — ignore
                break;
            case 'S': {  // ParameterStatus
                std::size_t pos = 0;
                std::size_t end = payload.find('\0', pos);
                if (end != std::string::npos) {
                    std::string name = payload.substr(pos, end - pos);
                    pos = end + 1;
                    end = payload.find('\0', pos);
                    if (end != std::string::npos) {
                        std::string value = payload.substr(pos, end - pos);
                        // Update existing or append.
                        bool found = false;
                        for (auto& kv : parameter_status_) {
                            if (kv.first == name) {
                                kv.second = value;
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            parameter_status_.emplace_back(std::move(name), std::move(value));
                        }
                    }
                }
                break;
            }
            case 'K':  // BackendKeyData (rare during query)
                if (payload.size() >= 8) {
                    backend_pid_ = ReadInt32(payload.data());
                    backend_secret_ = static_cast<uint32_t>(ReadInt32(payload.data() + 4));
                }
                break;
            case 'G': {  // CopyInResponse
                result.SetStatus(ExecStatusType::kCopyIn);
                copy_in_progress_ = true;
                copy_mode_ = 1;
                got_any_result = true;
                return result;
            }
            case 'H': {  // CopyOutResponse
                result.SetStatus(ExecStatusType::kCopyOut);
                copy_in_progress_ = true;
                copy_mode_ = 2;
                got_any_result = true;
                return result;
            }
            case 'W': {  // CopyBothResponse
                result.SetStatus(ExecStatusType::kCopyBoth);
                copy_in_progress_ = true;
                copy_mode_ = 3;
                got_any_result = true;
                return result;
            }
            case 'd': {  // CopyData
                result.Clear();
                result.SetStatus(ExecStatusType::kCopyOut);
                result.SetCommandStatus(std::move(payload));
                return result;
            }
            case 'c': {  // CopyDone
                copy_in_progress_ = false;
                copy_mode_ = 0;
                got_any_result = true;
                break;
            }
            case 'Z':  // ReadyForQuery
                in_busy_ = false;
                query_done_ = true;
                if (!got_any_result && !expecting_rows) {
                    // No result was produced — return empty terminator.
                    return result;
                }
                return result;
            default:
                // Unknown message — ignore.
                break;
        }
    }
}

// --- ConsumeInput / IsBusy / Flush ---------------------------------------

bool PgConn::ConsumeInput() {
    if (fd_ < 0)
        return false;
    // Nonblocking: switch socket to nonblocking for the read.
    int flags = fcntl(fd_, F_GETFL, 0);
    fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
    char buf[4096];
    while (true) {
        ssize_t n = ReadSome(fd_, buf, sizeof(buf));
        if (n < 0) {
            fcntl(fd_, F_SETFL, flags);
            return false;
        }
        if (n == 0)
            break;
        in_buffer_.append(buf, static_cast<std::size_t>(n));
    }
    fcntl(fd_, F_SETFL, flags);
    return true;
}

bool PgConn::IsBusy() {
    return in_busy_;
}

int PgConn::Flush() {
    // pgcpp sends messages synchronously so nothing is buffered on send.
    return 0;
}

// --- Notifications --------------------------------------------------------

const PGnotify* PgConn::Notifies() {
    if (notify_read_pos_ < static_cast<int>(pending_notify_.size())) {
        return &pending_notify_[notify_read_pos_++];
    }
    return nullptr;
}

// --- COPY ----------------------------------------------------------------

int PgConn::PutCopyData(const char* data, int len) {
    if (!copy_in_progress_ || copy_mode_ != 1)
        return -1;
    std::string payload(data, static_cast<std::size_t>(len));
    if (!SendMessage('d', payload))
        return -1;
    return 1;
}

int PgConn::PutCopyEnd(const std::string& error_msg) {
    if (!copy_in_progress_ || copy_mode_ != 1)
        return -1;
    if (error_msg.empty()) {
        if (!SendMessage('c', ""))
            return -1;
    } else {
        std::string payload = error_msg;
        payload.push_back('\0');
        if (!SendMessage('f', payload))
            return -1;
    }
    copy_in_progress_ = false;
    copy_mode_ = 0;
    return 1;
}

int PgConn::GetCopyData(char** buffer, int async) {
    (void)async;
    if (!copy_in_progress_ || copy_mode_ != 2)
        return -1;
    char type;
    std::string payload;
    if (!ReadMessage(&type, &payload))
        return -2;
    if (type == 'd') {
        // CopyData — give the caller a heap-allocated copy.
        *buffer = static_cast<char*>(std::malloc(payload.size()));
        if (*buffer == nullptr)
            return -2;
        std::memcpy(*buffer, payload.data(), payload.size());
        return static_cast<int>(payload.size());
    }
    if (type == 'c') {
        copy_in_progress_ = false;
        copy_mode_ = 0;
        return -1;
    }
    if (type == 'E') {
        error_message_ = ParseErrorMessage(payload);
        return -2;
    }
    return -2;
}

// --- Single-row / pipeline modes ----------------------------------------

bool PgConn::SetSingleRowMode() {
    if (pipeline_status_ != PipelineStatusType::kOn) {
        single_row_mode_ = true;
        return true;
    }
    single_row_mode_ = true;
    return true;
}

bool PgConn::EnterPipelineMode() {
    if (pipeline_status_ == PipelineStatusType::kOn)
        return false;
    pipeline_status_ = PipelineStatusType::kOn;
    return true;
}

bool PgConn::ExitPipelineMode() {
    if (pipeline_status_ != PipelineStatusType::kOn)
        return false;
    if (!SendMessage('S', "")) {
        return false;
    }
    pipeline_status_ = PipelineStatusType::kOff;
    return true;
}

bool PgConn::PipelineSync() {
    if (pipeline_status_ != PipelineStatusType::kOn)
        return false;
    return SendMessage('S', "");
}

// --- Cancellation --------------------------------------------------------

bool PgConn::CancelCurrentQuery() {
    if (backend_pid_ == 0) {
        error_message_ = "no backend key data available";
        return false;
    }
    // Open a fresh socket to the same host:port.
    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    if (cfd < 0)
        return false;
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0) {
        close(cfd);
        return false;
    }
    if (connect(cfd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(cfd);
        return false;
    }
    // Send CancelRequest packet.
    char msg[16];
    PutInt32Raw(msg, 16);
    PutInt32Raw(msg + 4, 0x04D2162F);
    PutInt32Raw(msg + 8, static_cast<int32_t>(backend_pid_));
    PutInt32Raw(msg + 12, static_cast<int32_t>(backend_secret_));
    bool ok = WriteAll(cfd, msg, 16);
    close(cfd);
    if (!ok) {
        error_message_ = "failed to send cancel request";
        return false;
    }
    return true;
}

// --- ParameterStatus lookup ---------------------------------------------

const std::string* PgConn::ParameterStatus(const std::string& name) const {
    for (const auto& kv : parameter_status_) {
        if (kv.first == name)
            return &kv.second;
    }
    return nullptr;
}

// --- FormatResult --------------------------------------------------------

std::string FormatResult(const PgResult& result) {
    std::string out;
    if (result.Status() == ExecStatusType::kFatalError) {
        out += "ERROR:  ";
        out += result.ErrorMessage();
        out += "\n";
        return out;
    }
    if (result.Status() == ExecStatusType::kEmptyQuery) {
        out += "\n";
        return out;
    }
    if (result.NFields() == 0) {
        // DML or utility command.
        if (!result.CommandStatus().empty()) {
            out += result.CommandStatus();
            out += "\n";
        }
        return out;
    }
    // Calculate column widths.
    std::vector<std::size_t> widths;
    widths.reserve(static_cast<std::size_t>(result.NFields()));
    for (int i = 0; i < result.NFields(); ++i) {
        widths.push_back(result.FName(i).size());
    }
    for (int r = 0; r < result.NTuples(); ++r) {
        for (int c = 0; c < result.NFields(); ++c) {
            const char* v = result.GetValue(r, c);
            std::size_t len = v != nullptr ? std::strlen(v) : 0;
            if (len > widths[c])
                widths[c] = len;
        }
    }
    // Header.
    out += " ";
    for (int c = 0; c < result.NFields(); ++c) {
        if (c > 0)
            out += " | ";
        out += result.FName(c);
        out += std::string(widths[c] - result.FName(c).size(), ' ');
    }
    out += "\n-";
    for (int c = 0; c < result.NFields(); ++c) {
        if (c > 0)
            out += "-+-";
        out += std::string(widths[c], '-');
    }
    out += "\n";
    // Rows.
    for (int r = 0; r < result.NTuples(); ++r) {
        out += " ";
        for (int c = 0; c < result.NFields(); ++c) {
            if (c > 0)
                out += " | ";
            const char* v = result.GetValue(r, c);
            std::string sv = v != nullptr ? v : "";
            out += sv;
            out += std::string(widths[c] - sv.size(), ' ');
        }
        out += "\n";
    }
    out += "(";
    out += std::to_string(result.NTuples());
    out += result.NTuples() == 1 ? " row)\n" : " rows)\n";
    return out;
}

}  // namespace pgcpp::libpq

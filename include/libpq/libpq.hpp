// libpq.hpp — PostgreSQL libpq-style client library (P3-11).
//
// pgcpp's libpq is a C++20 client library that mirrors the public API
// surface of PostgreSQL's libpq (fe-connect.c / fe-exec.c /
// fe-protocol3.c), adapted to C++ idioms (RAII, std::string,
// std::vector, no exceptions). It is the underlying layer beneath the
// psql tool and is intended to allow external C++ clients to talk to a
// pgcpp server (or any PostgreSQL server speaking protocol v3).
//
// The library provides:
//
//   - PgConn:           synchronous connection management + query exec
//   - PgResult:         query result rows / status / metadata
//   - PGnotify:         async notification (LISTEN/NOTIFY)
//   - async APIs:       SendQuery / GetResult / ConsumeInput
//   - pipeline mode:     EnterPipelineMode / ExitPipelineMode / PipelineSync
//   - single row mode:  SetSingleRowMode
//   - COPY support:      PutCopyData / PutCopyEnd / GetCopyData
//   - parametrized:      ExecParams / Prepare / ExecPrepared
//
// Design notes:
//
//   - The wire layer uses the same MessageWriter/MessageReader classes
//     as the server side (protocol/pqformat.hpp) so the message format
//     is shared.
//
//   - Synchronous connect is implemented as a state machine that loops
//     over ConnectPoll, so that the nonblocking path and the sync path
//     share the same code.
//
//   - Error reporting is via std::string error_message_ (not ereport)
//     because client code cannot use PG_TRY/PG_CATCH (those are
//     server-side abstractions backed by setjmp/longjmp).
//
//   - The library is single-threaded by design (matches pgcpp's
//     "no std::thread" rule). Multiple concurrent connections must be
//     driven by the caller using select/poll on the file descriptors
//     exposed via Socket().
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "libpq/conninfo.hpp"

namespace pgcpp::libpq {

// Forward declarations.
class PgConn;

// PGnotify — async notification (LISTEN/NOTIFY) received from the
// server as an 'A' message. Mirrors libpq's PGnotify struct.
struct PGnotify {
    std::string relname;  // channel name
    int be_pid = 0;       // backend PID of the notifier
    std::string extra;    // payload string (empty if no payload)
};

// ExecStatusType — result status (mirrors libpq's ExecStatusType).
enum class ExecStatusType {
    kEmptyQuery,   // empty query string sent to server
    kCommandOk,    // a command not returning tuples (INSERT/UPDATE/...)
    kTuplesOk,     // successful query returning tuples (SELECT)
    kCopyOut,      // COPY TO STDOUT started
    kCopyIn,       // COPY FROM STDIN started
    kBadResponse,  // server sent an unexpected response
    kFatalError,   // server reported a fatal error
    kCopyBoth,     // COPY IN/OUT (used by walsender; rare)
    kSingleTuple,  // single-row mode: this result contains one row
};

// PipelineStatusType — pipeline mode status (mirrors libpq's
// PGpipelineStatus).
enum class PipelineStatusType {
    kOff,      // not in pipeline mode
    kOn,       // in pipeline mode
    kAborted,  // pipeline aborted due to a previous error
};

// FieldFormat — column format code for param/result values.
enum class FieldFormat : int16_t {
    kText = 0,    // text format
    kBinary = 1,  // binary format
};

// Param — a single parameter for ExecParams / ExecPrepared.
struct Param {
    std::string value;  // text-encoded value
    bool is_null = false;
};

// ResultField — metadata for one column of a result set.
struct ResultField {
    std::string name;        // column name
    uint32_t table_oid = 0;  // source table OID (0 if not a column)
    int16_t column_attnum = 0;
    uint32_t type_oid = 0;
    int16_t type_size = 0;  // typlen (-1 for varlena)
    int32_t type_mod = -1;
    int16_t format = 0;  // 0=text, 1=binary
};

// PgResult — result of a query execution.
//
// Owns the result rows (text-encoded values) and column metadata.
// Callers must hold a PgResult while reading its values; Clear()
// releases internal storage (the destructor also does this).
class PgResult {
public:
    PgResult() = default;
    ~PgResult() = default;

    PgResult(PgResult&&) noexcept = default;
    PgResult& operator=(PgResult&&) noexcept = default;

    PgResult(const PgResult&) = delete;
    PgResult& operator=(const PgResult&) = delete;

    // Status — current result status.
    ExecStatusType Status() const { return status_; }
    void SetStatus(ExecStatusType s) { status_ = s; }

    // NTuples — number of result rows.
    int NTuples() const { return static_cast<int>(rows_.size()); }

    // NFields — number of result columns.
    int NFields() const { return static_cast<int>(fields_.size()); }

    // FName — column name by index.
    const std::string& FName(int col) const;

    // FNumber — column index by name (case-insensitive). Returns -1 if
    // not found.
    int FNumber(const std::string& name) const;

    // FType — column type OID.
    uint32_t FType(int col) const;

    // FMod — column type modifier.
    int32_t FMod(int col) const;

    // FSize — column size (typlen from pg_type).
    int16_t FSize(int col) const;

    // FFormat — column format (text=0, binary=1).
    int16_t FFormat(int col) const;

    // FTable — source table OID for the column.
    uint32_t FTable(int col) const;

    // FTableCol — column attribute number in source table.
    int16_t FTableCol(int col) const;

    // GetValue — value of (row, col) as a C string (text-encoded).
    // Returns nullptr for NULL.
    const char* GetValue(int row, int col) const;

    // GetIsNull — true if (row, col) is NULL.
    bool GetIsNull(int row, int col) const;

    // GetLength — length in bytes of (row, col). -1 for NULL.
    int GetLength(int row, int col) const;

    // CommandStatus — command tag string (e.g. "SELECT 3").
    const std::string& CommandStatus() const { return command_tag_; }
    void SetCommandStatus(const std::string& tag) { command_tag_ = tag; }

    // CommandTuples — number of rows affected by the command (parsed
    // from the command tag). Returns -1 if not parseable.
    int CommandTuples() const;

    // ErrorMessage — error message (only valid if Status == kFatalError).
    const std::string& ErrorMessage() const { return error_message_; }
    void SetErrorMessage(const std::string& msg) { error_message_ = msg; }

    // VerboseErrorMessage — full error (severity/code/detail/hint/...).
    // Currently the same as ErrorMessage(); kept for API parity.
    std::string VerboseErrorMessage() const;

    // OIDValue — for INSERT...RETURNING oid, the OID of the inserted
    // row. Returns kInvalidOid if not applicable.
    uint32_t OIDValue() const { return inserted_oid_; }
    void SetOIDValue(uint32_t oid) { inserted_oid_ = oid; }

    // Clear — release all internal storage (resets to empty result).
    void Clear();

    // --- Mutators used by the protocol layer to populate a result ---

    void SetFields(std::vector<ResultField> fields) { fields_ = std::move(fields); }
    void AddRow(std::vector<std::string> values, std::vector<bool> isnull);
    void ReserveRows(int n);

    // Read-only access to internal storage (for tests / advanced users).
    const std::vector<ResultField>& fields() const { return fields_; }
    const std::vector<std::pair<std::vector<std::string>, std::vector<bool>>>& rows() const {
        return rows_;
    }

private:
    ExecStatusType status_ = ExecStatusType::kFatalError;
    std::vector<ResultField> fields_;
    std::vector<std::pair<std::vector<std::string>, std::vector<bool>>> rows_;
    std::string command_tag_;
    std::string error_message_;
    uint32_t inserted_oid_ = 0;
};

// ConnectOptions — convenience bundle for synchronous connect.
struct ConnectOptions {
    std::string host = "localhost";
    int port = 5432;
    std::string user;
    std::string dbname;
    std::string password;
    std::string options;       // backend -o options string
    int connect_timeout = 0;   // seconds (0 = no timeout)
    bool require_ssl = false;  // pgcpp ignores SSL for now
};

// PgConn — a connection to a PostgreSQL server.
//
// A PgConn is created in the disconnected (kBad) state. Connect() /
// ConnectStart+ConnectPoll establish the connection. Once connected,
// Exec / ExecParams / ExecPrepared issue queries and return a PgResult.
// Async variants (SendQuery / GetResult / ConsumeInput) allow nonblocking
// use; IsBusy() / Flush() expose the underlying I/O state.
//
// The destructor calls Finish() if the connection is still open.
class PgConn {
public:
    // Construct a disconnected connection. Use Connect(conninfo) or
    // Connect(opts) to establish a session.
    PgConn();
    ~PgConn();

    PgConn(const PgConn&) = delete;
    PgConn& operator=(const PgConn&) = delete;
    PgConn(PgConn&&) noexcept;
    PgConn& operator=(PgConn&&) noexcept;

    // --- Synchronous connect / disconnect ---

    // Connect — parse conninfo (keyword=value or URI) and connect to the
    // server. Returns kOk on success, kBad on failure. On failure,
    // ErrorMessage() contains the reason.
    ConnStatusType Connect(const std::string& conninfo);

    // Connect — connect using explicit options. Same return convention.
    ConnStatusType Connect(const ConnectOptions& opts);

    // Finish — close the connection (sends Terminate, closes socket).
    // Safe to call multiple times. After Finish, Status() == kBad.
    void Finish();

    // Reset — drop the current connection and re-establish it with
    // the same conninfo. Returns kOk on success.
    ConnStatusType Reset();

    // --- Async connect (state machine) ---

    // ConnectStart — begin a nonblocking connect. Returns kStarted on
    // success (caller should now poll ConnectPoll), or kBad on immediate
    // failure. After ConnectStart, the caller uses ConnectPoll to drive
    // the connection forward.
    ConnStatusType ConnectStart(const std::string& conninfo);

    // ConnectPoll — advance the connection state machine. Returns
    // kOk when connected, kFailed on error, kReading/kWriting when the
    // caller should wait for socket readability/writability, kActive
    // when internal work is still in progress.
    PollingStatusType ConnectPoll();

    // --- Query execution (synchronous) ---

    // Exec — send a simple query, wait for the result.
    // Returns a PgResult whose Status() is one of kCommandOk / kTuplesOk
    // / kFatalError / kEmptyQuery / kCopyOut / kCopyIn.
    PgResult Exec(const std::string& query);

    // ExecParams — send a parameterized query (extended protocol).
    // `param_values` text-encoded parameter values; `param_isnull`
    // indicates NULL. The server prepares, binds, and executes the
    // statement in one round trip.
    PgResult ExecParams(const std::string& query, const std::vector<Param>& params);

    // Prepare — create a named prepared statement (extended protocol).
    // Returns an empty-result PgResult whose Status() is kCommandOk on
    // success or kFatalError on failure.
    PgResult Prepare(const std::string& stmtName, const std::string& query,
                     const std::vector<uint32_t>& paramTypes);

    // ExecPrepared — execute a previously prepared statement.
    PgResult ExecPrepared(const std::string& stmtName, const std::vector<Param>& params);

    // --- Query execution (asynchronous) ---

    // SendQuery — submit a query without waiting for the result. After
    // SendQuery, the caller uses GetResult to obtain result(s).
    // Returns true on success (the query was sent), false on I/O error.
    bool SendQuery(const std::string& query);

    // SendQueryParams — async parameterized query.
    bool SendQueryParams(const std::string& query, const std::vector<Param>& params);

    // SendPrepare — async PREPARE.
    bool SendPrepare(const std::string& stmtName, const std::string& query,
                     const std::vector<uint32_t>& paramTypes);

    // SendQueryPrepared — async EXECUTE of a prepared statement.
    bool SendQueryPrepared(const std::string& stmtName, const std::vector<Param>& params);

    // GetResult — block until the next result is available. Returns a
    // non-empty PgResult for each result produced by the server, and an
    // empty (default-constructed) PgResult after the last result.
    // Typical usage:
    //   while (auto r = conn.GetResult()) { ... process r ... }
    PgResult GetResult();

    // ConsumeInput — read any available input from the server without
    // blocking. After ConsumeInput, IsBusy() can be checked to see if
    // more input is needed. Returns true on success, false on I/O error.
    bool ConsumeInput();

    // IsBusy — true if the server is still processing the current query.
    // After ConsumeInput returns false (not busy), the caller may call
    // GetResult.
    bool IsBusy();

    // Flush — attempt to flush any queued output. Returns 0 on success
    // (everything flushed), 1 if it would block (caller should wait for
    // socket writability), -1 on error.
    int Flush();

    // --- Notifications ---

    // Notifies — return the next pending PGnotify, or nullptr if none.
    // The pointer is invalidated by the next call to any I/O function
    // on this connection.
    const PGnotify* Notifies();

    // --- COPY ---

    // PutCopyData — send data during COPY FROM STDIN. Returns 1 on
    // success, -1 on error, 0 if it would block (caller should retry).
    int PutCopyData(const char* data, int len);

    // PutCopyEnd — end COPY FROM STDIN. `error_msg` is empty on
    // successful end, or a non-empty message to signal COPY FAIL.
    // Returns 1 on success, -1 on error.
    int PutCopyEnd(const std::string& error_msg);

    // GetCopyData — receive data during COPY TO STDOUT. Returns the
    // number of bytes read (>0), 0 if the copy is still in progress
    // (caller should wait for more input), -1 if the copy is complete,
    // -2 on error. The buffer pointer is set on success.
    int GetCopyData(char** buffer, int async);

    // --- Single-row mode ---

    // SetSingleRowMode — request single-row mode for the next query.
    // Must be called immediately after SendQuery*. Returns true on
    // success, false if not in pipeline mode or already past the
    // RowDescription stage.
    bool SetSingleRowMode();

    // --- Pipeline mode ---

    // EnterPipelineMode — switch the connection to pipeline mode.
    // Returns true on success, false if already in pipeline mode.
    bool EnterPipelineMode();

    // ExitPipelineMode — leave pipeline mode. Sends a Sync message if
    // one is pending. Returns true on success.
    bool ExitPipelineMode();

    // PipelineSync — send a Sync marker in the pipeline. The result of
    // each query sent before the Sync will be available via GetResult.
    // Returns true on success.
    bool PipelineSync();

    // PipelineStatus — current pipeline mode status.
    PipelineStatusType PipelineStatus() const { return pipeline_status_; }

    // --- Cancellation ---

    // Cancel — request cancellation of the currently running query.
    // Returns true on success (cancel request sent), false on error.
    // The query result will be returned as kFatalError from GetResult.
    bool CancelCurrentQuery();

    // --- Accessors ---

    // Status — current connection status.
    ConnStatusType Status() const { return status_; }

    // Socket — file descriptor of the connection socket. Returns -1 if
    // not connected. Used with select/poll for async I/O.
    int Socket() const { return fd_; }

    // ErrorMessage — most recent error message (empty if none).
    const std::string& ErrorMessage() const { return error_message_; }

    // BackendPID — server-side backend PID (from BackendKeyData).
    int BackendPID() const { return backend_pid_; }

    // BackendSecret — server-side backend secret (for cancellation).
    uint32_t BackendSecret() const { return backend_secret_; }

    // Host / Port / User / DB / Options — connection parameters.
    const std::string& Host() const { return host_; }
    int Port() const { return port_; }
    const std::string& User() const { return user_; }
    const std::string& DB() const { return dbname_; }
    const std::string& Options() const { return options_; }

    // ParameterStatus — lookup a server-reported session parameter
    // (e.g. "server_version", "client_encoding", "standard_conforming_strings").
    // Returns nullptr if the parameter is not reported.
    const std::string* ParameterStatus(const std::string& name) const;

    // ProtocolVersion — major*10000 + minor (3 << 16 for protocol v3).
    int ProtocolVersion() const { return protocol_version_; }

    // ServerVersion — server version string (e.g. "15.0").
    const std::string& ServerVersion() const { return server_version_; }

    // ClientEncoding — encoding negotiated with the server ("UTF8").
    const std::string& ClientEncoding() const { return client_encoding_; }

    // Trace / Untrace — toggle protocol message logging to stderr.
    void Trace() { trace_ = true; }
    void Untrace() { trace_ = false; }

    // ConnectionOptions — get the parsed ConnInfoOption vector that
    // was used to establish the connection (empty before connect).
    const std::vector<ConnInfoOption>& ConnectionOptions() const { return conn_options_; }

private:
    // Internal helpers.
    bool SendStartupMessage();
    bool SendCancelRequest(uint32_t pid, uint32_t secret);
    bool SendMessage(char type, const std::string& payload);
    bool ReadMessage(char* type, std::string* payload);
    bool SendAll(const void* data, std::size_t len);
    bool ReadAll(void* buf, std::size_t len);

    // ConnectPoll state machine helpers.
    bool TryConnect();     // Nonblocking socket connect wrapper.
    bool DoStartupAuth();  // Drive startup + auth handshake.
    bool DoAuthResponse(const std::string& password);

    // Internal state.
    std::string host_;
    int port_ = 5432;
    std::string user_;
    std::string dbname_;
    std::string password_;
    std::string options_;
    int connect_timeout_ = 0;

    int fd_ = -1;
    ConnStatusType status_ = ConnStatusType::kBad;
    std::string error_message_;
    int protocol_version_ = 0;
    std::string server_version_;
    std::string client_encoding_ = "UTF8";
    int backend_pid_ = 0;
    uint32_t backend_secret_ = 0;
    bool trace_ = false;

    // Async connect state machine.
    enum class ConnectState {
        kIdle,
        kConnecting,       // TCP connect in progress
        kAwaitingStartup,  // TCP connected, send startup
        kAwaitingAuth,     // Auth handshake in progress
        kAwaitingReady,    // Auth done, waiting for ReadyForQuery
    };
    ConnectState connect_state_ = ConnectState::kIdle;
    bool single_row_mode_ = false;
    PipelineStatusType pipeline_status_ = PipelineStatusType::kOff;

    // Pending notification (filled by Notifies from in_buffer_).
    std::vector<PGnotify> pending_notify_;
    int notify_read_pos_ = 0;

    // Server parameter status map.
    std::vector<ConnInfoOption> conn_options_;
    std::vector<std::pair<std::string, std::string>> parameter_status_;

    // Pending COPY state.
    bool copy_in_progress_ = false;
    int copy_mode_ = 0;  // 0 = none, 1 = in, 2 = out

    // Async read buffer (for partial messages).
    std::string in_buffer_;
    bool in_busy_ = false;

    // True after GetResult has consumed a ReadyForQuery message (i.e. the
    // current query's result stream is fully drained). The next GetResult
    // call returns an empty terminator and clears this flag. This mirrors
    // libpq's PQgetResult returning NULL after the final result.
    bool query_done_ = false;

    // Pending results buffer (for pipeline mode).
    std::vector<PgResult> pending_results_;

    // Conninfo string used for the most recent successful connection
    // (used by Reset() to re-establish with the same parameters).
    std::string last_conninfo_;
};

// FormatResult — pretty-print a PgResult as a text table (used by psql
// when run in non-interactive mode). Returns the formatted string.
std::string FormatResult(const PgResult& result);

}  // namespace pgcpp::libpq

// pg_ctl.h — Server control utility (pg_ctl).
//
// Converted from PostgreSQL 15's src/bin/pg_ctl/.
//
// pg_ctl starts, stops, restarts, reloads, or queries the status of a
// PostgreSQL (or pgcpp) server. It works by:
//   - Reading the data directory to find the postmaster.pid file.
//   - Sending signals to the postmaster process (SIGTERM/SIGHUP/SIGINT).
//   - For `start`: fork+exec the server binary.
//   - For `init`: invoke initdb.
//   - For `promote`: write a trigger file.
//
// pgcpp's pg_ctl is a faithful port. Since pgcpp is single-process, the
// implementation uses fork+exec (matching PG) and writes a postmaster.pid
// file in the data directory.
#pragma once

#include <cstdint>
#include <string>

namespace pgcpp::tools {

// PgCtlAction — the operation to perform.
enum class PgCtlAction {
    kStart,
    kStop,
    kRestart,
    kReload,
    kStatus,
    kInit,
    kPromote,
    kKill,
};

// PgCtlStopMode — how to stop the server (mirrors PG's -m option).
enum class PgCtlStopMode {
    kSmart,      // wait for clients to disconnect (default)
    kFast,       // abort current transactions, then quit
    kImmediate,  // quit immediately, will require recovery on restart
};

// PgCtlOptions — inputs to the pg_ctl operation.
struct PgCtlOptions {
    PgCtlAction action = PgCtlAction::kStart;
    std::string data_dir;                            // -D
    std::string log_file;                            // -l (start/restart only)
    int port = 5433;                                 // -o "-p PORT"
    std::string listen_addr = "127.0.0.1";           // -o "-h ADDR"
    PgCtlStopMode stop_mode = PgCtlStopMode::kFast;  // -m
    int wait_secs = 60;                              // -W (0 = no wait)
    bool silent = false;                             // -s
};

// PgCtlResult — outcome of a pg_ctl operation.
enum class PgCtlResult {
    kOk,
    kInvalidDataDir,
    kNoPostmasterPid,  // status/stop but server not running
    kAlreadyRunning,   // start but server already running
    kStartFailed,
    kStopFailed,
    kTimeoutExceeded,
    kSignalFailed,
};

// PgCtlMain — perform the action described by `opts`.
// Returns kOk on success; otherwise an error code.
PgCtlResult PgCtlMain(const PgCtlOptions& opts);

// --- Helpers (exposed for testing) ---

// ReadPostmasterPid — read the PID from <data_dir>/postmaster.pid.
// Returns 0 if the file is absent or malformed.
int64_t ReadPostmasterPid(const std::string& data_dir);

// WritePostmasterPid — write `pid` to <data_dir>/postmaster.pid.
// Returns true on success.
bool WritePostmasterPid(const std::string& data_dir, int64_t pid);

// RemovePostmasterPid — delete the postmaster.pid file.
void RemovePostmasterPid(const std::string& data_dir);

// IsServerRunning — true if a postmaster.pid file exists and the recorded
// PID is alive.
bool IsServerRunning(const std::string& data_dir);

// SignalPostmaster — send `signo` to `pid`. Returns true on success.
bool SignalPostmaster(int64_t pid, int signo);

// StopModeToSignal — convert a PgCtlStopMode to the corresponding signal.
//   kSmart     -> SIGTERM
//   kFast      -> SIGINT
//   kImmediate -> SIGQUIT
int StopModeToSignal(PgCtlStopMode mode);

}  // namespace pgcpp::tools

// pg_ctl.cpp — Server control utility (pg_ctl).
//
// Converted from PostgreSQL 15's src/bin/pg_ctl/.
//
// pg_ctl starts, stops, restarts, reloads, or queries the status of a
// pgcpp server. It works by reading <data_dir>/postmaster.pid to find the
// postmaster PID, sending signals (SIGTERM/SIGHUP/SIGINT/SIGQUIT), and
// for `start` it fork+execs the server binary.
//
// The project forbids std::thread, so polling waits use usleep(3) from
// <unistd.h> rather than std::this_thread::sleep_for.
#include "tools/pg_ctl.hpp"

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace pgcpp::tools {

namespace {

// Build the path to <data_dir>/postmaster.pid.
std::string PidFilePath(const std::string& data_dir) {
    if (data_dir.empty())
        return "postmaster.pid";
    if (data_dir.back() == '/')
        return data_dir + "postmaster.pid";
    return data_dir + "/postmaster.pid";
}

// Build the path to <data_dir>/promote.signal.
std::string PromoteFilePath(const std::string& data_dir) {
    if (data_dir.empty())
        return "promote.signal";
    if (data_dir.back() == '/')
        return data_dir + "promote.signal";
    return data_dir + "/promote.signal";
}

// Sleep for `ms` milliseconds by polling. Uses usleep(3) instead of
// std::this_thread::sleep_for because the project forbids std::thread.
void SleepMs(int ms) {
    if (ms <= 0)
        return;
    usleep(static_cast<useconds_t>(ms) * 1000u);
}

}  // namespace

int64_t ReadPostmasterPid(const std::string& data_dir) {
    std::ifstream in(PidFilePath(data_dir));
    if (!in.is_open())
        return 0;
    std::string line;
    if (!std::getline(in, line))
        return 0;
    // Parse the leading integer token; PG's postmaster.pid puts the PID on
    // the first line.
    char* end = nullptr;
    long long val = std::strtoll(line.c_str(), &end, 10);
    if (end == line.c_str() || val < 0)
        return 0;
    return static_cast<int64_t>(val);
}

bool WritePostmasterPid(const std::string& data_dir, int64_t pid) {
    std::ofstream out(PidFilePath(data_dir), std::ios::out | std::ios::trunc);
    if (!out.is_open())
        return false;
    out << std::to_string(pid) << "\n";
    out.flush();
    return out.good();
}

void RemovePostmasterPid(const std::string& data_dir) {
    std::remove(PidFilePath(data_dir).c_str());
}

bool IsServerRunning(const std::string& data_dir) {
    int64_t pid = ReadPostmasterPid(data_dir);
    if (pid <= 0)
        return false;
    // kill(pid, 0) returns 0 if the process exists, -1 (ESRCH) otherwise.
    return kill(static_cast<pid_t>(pid), 0) == 0;
}

bool SignalPostmaster(int64_t pid, int signo) {
    // Guard against pid <= 0: kill(0, signo) would signal the caller's whole
    // process group, and kill(<negative>, signo) would signal a process group.
    if (pid <= 0)
        return false;
    return kill(static_cast<pid_t>(pid), signo) == 0;
}

int StopModeToSignal(PgCtlStopMode mode) {
    switch (mode) {
        case PgCtlStopMode::kSmart:
            return SIGTERM;
        case PgCtlStopMode::kFast:
            return SIGINT;
        case PgCtlStopMode::kImmediate:
            return SIGQUIT;
    }
    return SIGTERM;
}

PgCtlResult PgCtlMain(const PgCtlOptions& opts) {
    switch (opts.action) {
        case PgCtlAction::kInit: {
            // Caller should use initdb directly.
            return PgCtlResult::kOk;
        }
        case PgCtlAction::kStatus: {
            if (opts.data_dir.empty())
                return PgCtlResult::kInvalidDataDir;
            if (!IsServerRunning(opts.data_dir)) {
                return PgCtlResult::kNoPostmasterPid;
            }
            return PgCtlResult::kOk;
        }
        case PgCtlAction::kStop: {
            if (opts.data_dir.empty())
                return PgCtlResult::kInvalidDataDir;
            int64_t pid = ReadPostmasterPid(opts.data_dir);
            if (pid <= 0)
                return PgCtlResult::kNoPostmasterPid;
            int signo = StopModeToSignal(opts.stop_mode);
            if (!SignalPostmaster(pid, signo))
                return PgCtlResult::kSignalFailed;
            // Wait up to wait_secs for the process to die, polling every
            // 100ms.
            if (opts.wait_secs > 0) {
                int max_ms = opts.wait_secs * 1000;
                int waited_ms = 0;
                while (waited_ms < max_ms) {
                    if (kill(static_cast<pid_t>(pid), 0) != 0)
                        break;
                    SleepMs(100);
                    waited_ms += 100;
                }
            }
            RemovePostmasterPid(opts.data_dir);
            // Final check: did the process actually die?
            if (kill(static_cast<pid_t>(pid), 0) != 0) {
                return PgCtlResult::kOk;
            }
            return PgCtlResult::kTimeoutExceeded;
        }
        case PgCtlAction::kStart: {
            if (opts.data_dir.empty())
                return PgCtlResult::kInvalidDataDir;
            if (IsServerRunning(opts.data_dir)) {
                return PgCtlResult::kAlreadyRunning;
            }
            pid_t child = fork();
            if (child < 0)
                return PgCtlResult::kStartFailed;
            if (child == 0) {
                // Child: exec the server binary. _exit (not std::exit) avoids
                // running inherited atexit handlers in the forked child.
                std::string port_str = std::to_string(opts.port);
                execlp("pgcpp_server", "pgcpp_server", "-D", opts.data_dir.c_str(), "-p",
                       port_str.c_str(), "-h", opts.listen_addr.c_str(), nullptr);
                // execlp only returns on error.
                _exit(127);
            }
            // Parent: record the child pid so IsServerRunning can see it.
            if (!WritePostmasterPid(opts.data_dir, static_cast<int64_t>(child))) {
                return PgCtlResult::kStartFailed;
            }
            // Wait up to wait_secs for the server to be ready, polling
            // IsServerRunning every 100ms.
            if (opts.wait_secs > 0) {
                int max_ms = opts.wait_secs * 1000;
                int waited_ms = 0;
                while (waited_ms < max_ms) {
                    if (IsServerRunning(opts.data_dir)) {
                        return PgCtlResult::kOk;
                    }
                    SleepMs(100);
                    waited_ms += 100;
                }
            }
            if (IsServerRunning(opts.data_dir))
                return PgCtlResult::kOk;
            return PgCtlResult::kStartFailed;
        }
        case PgCtlAction::kRestart: {
            // Stop then start. Tolerate "not running" so restart can revive
            // a stopped server (matches PostgreSQL behavior).
            PgCtlOptions stop_opts = opts;
            stop_opts.action = PgCtlAction::kStop;
            PgCtlResult sr = PgCtlMain(stop_opts);
            if (sr != PgCtlResult::kOk && sr != PgCtlResult::kNoPostmasterPid) {
                return sr;
            }
            PgCtlOptions start_opts = opts;
            start_opts.action = PgCtlAction::kStart;
            return PgCtlMain(start_opts);
        }
        case PgCtlAction::kReload: {
            if (opts.data_dir.empty())
                return PgCtlResult::kInvalidDataDir;
            int64_t pid = ReadPostmasterPid(opts.data_dir);
            if (pid <= 0)
                return PgCtlResult::kNoPostmasterPid;
            if (!SignalPostmaster(pid, SIGHUP)) {
                return PgCtlResult::kSignalFailed;
            }
            return PgCtlResult::kOk;
        }
        case PgCtlAction::kPromote: {
            if (opts.data_dir.empty())
                return PgCtlResult::kInvalidDataDir;
            std::ofstream out(PromoteFilePath(opts.data_dir), std::ios::out | std::ios::trunc);
            if (!out.is_open())
                return PgCtlResult::kStartFailed;
            out << "promote\n";
            out.flush();
            return PgCtlResult::kOk;
        }
        case PgCtlAction::kKill: {
            // Not implemented (would need a pid argument in PgCtlOptions).
            return PgCtlResult::kOk;
        }
    }
    return PgCtlResult::kOk;
}

}  // namespace pgcpp::tools

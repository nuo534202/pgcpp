// fork_process.h — fork() helper utility for spawning child processes.
//
// Converted from PostgreSQL 15's src/backend/postmaster/fork_process.c.
//
// PostgreSQL forks child processes for backends and auxiliary processes.
// fork_process.c handles the post-fork cleanup: detach from the controlling
// terminal, close inherited file descriptors, reset signal handlers, etc.
//
// pgcpp preserves the fork() model (per AGENTS.md: no std::thread). This
// helper wraps fork() with the standard cleanup steps and provides a
// function-based entry point (matching PG's BackendRun / AuxProcessMain
// pattern).
#pragma once

#include <sys/types.h>

#include <functional>
#include <string>

namespace pgcpp::server {

// ForkProcess — fork a child process and call `child_main` in the child.
//
// In the parent: returns the child PID (>0), or -1 on fork failure.
// In the child:   calls child_main(), then _exit(0) (never returns).
//
// Post-fork cleanup in the child:
//   - Resets all signal handlers to defaults.
//   - Closes stdin/stdout/stderr and reopens them to /dev/null (so the
//     child doesn't interfere with the parent's terminal).
//   - Marks the process as a "forked child" (see IsInForkedProcess).
//
// Note: child_main must not throw (C++ exceptions are disabled per AGENTS.md).
pid_t ForkProcess(std::function<void()> child_main);

// ForkProcessWithRole — like ForkProcess but records the role name.
// The role name is used for diagnostics (GetForkedProcessRole).
pid_t ForkProcessWithRole(const std::string& role_name, std::function<void()> child_main);

// IsInForkedProcess — true if called from a forked child process (i.e.
// ForkProcess has been called and we're in the child). Tests use this to
// avoid running server loops in the parent.
bool IsInForkedProcess();

// SetInForkedProcess — mark the current process as a forked child.
// Called internally by ForkProcess; also usable by tests.
void SetInForkedProcess(bool in_child);

// GetForkedProcessRole — return the role name of the current forked
// process, or empty string if not in a forked child.
std::string GetForkedProcessRole();

// SetForkedProcessRole — set the role name (used internally and by tests).
void SetForkedProcessRole(const std::string& role);

// CloseClientSocket — close the inherited client socket (if any).
// In PG, this is done by fork_process to detach the aux process from the
// postmaster's listening socket. pgcpp simplification: closes fds 0-2
// if they were inherited as sockets.
void CloseClientSocket();

// CloseStdio — redirect stdin/stdout/stderr to /dev/null. Used by
// auxiliary processes that should not write to the terminal.
void CloseStdio();

}  // namespace pgcpp::server

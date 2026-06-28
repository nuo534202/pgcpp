// fork_process.cpp — fork() helper utility.
//
// Converted from PostgreSQL 15's src/backend/postmaster/fork_process.c.
//
// Wraps fork() with the standard post-fork cleanup: reset signal handlers,
// detach from controlling terminal, redirect stdio to /dev/null. Records
// the role name for diagnostics.
#include "mytoydb/server/fork_process.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <utility>

#include "mytoydb/server/interrupt.hpp"

namespace mytoydb::server {

namespace {

// File-scope state (function-returning-static pattern for Google C++ Style).
bool& InForkedChild() {
    static bool in_child = false;
    return in_child;
}

std::string& ChildRole() {
    static std::string role;
    return role;
}

}  // namespace

pid_t ForkProcess(std::function<void()> child_main) {
    return ForkProcessWithRole("", std::move(child_main));
}

pid_t ForkProcessWithRole(const std::string& role_name, std::function<void()> child_main) {
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid > 0) {
        // Parent: return the child PID.
        return pid;
    }

    // Child: perform cleanup and run the main function.
    InForkedChild() = true;
    ChildRole() = role_name;

    // Reset all signal handlers to defaults.
    ResetBackendSignalHandlers();

    // Redirect stdio to /dev/null (detach from terminal).
    CloseStdio();

    // Run the child's main function.
    if (child_main) {
        child_main();
    }

    // Exit immediately — never return to the caller (which is the parent's
    // stack, copied by fork but no longer valid in the child's context).
    _exit(0);
}

bool IsInForkedProcess() {
    return InForkedChild();
}

void SetInForkedProcess(bool in_child) {
    InForkedChild() = in_child;
}

std::string GetForkedProcessRole() {
    return ChildRole();
}

void SetForkedProcessRole(const std::string& role) {
    ChildRole() = role;
}

void CloseClientSocket() {
    // MyToyDB simplification: close fds 0-2 (stdin/stdout/stderr) if they
    // were inherited as sockets. In PG this closes the postmaster's listening
    // socket and the accepted client socket.
    // We don't track inherited fds explicitly, so this is effectively a no-op
    // (CloseStdio handles the stdio redirection).
}

void CloseStdio() {
    int devnull = open("/dev/null", O_RDWR);
    if (devnull < 0) {
        return;
    }
    // Duplicate /dev/null onto stdin, stdout, stderr.
    dup2(devnull, 0);
    dup2(devnull, 1);
    dup2(devnull, 2);
    if (devnull > 2) {
        close(devnull);
    }
}

}  // namespace mytoydb::server

// pqsignal.h — Signal-handling utilities (pqsignal.c).
//
// Converted from PostgreSQL 15's src/backend/libpq/pqsignal.c.
//
// PG provides a portable wrapper around sigaction(2):
//   pqsignal(signo, handler)  — install a handler, returning the previous one
//   pq_block_sigalrm()        — block SIGALRM (for critical sections)
//   pq_reset_sigalrm()        — restore previous SIGALRM mask
//   PG_SETMASK(&mask)         — set the signal mask (POSIX) or sigset (SYSV)
//
// pgcpp runs on Linux only and uses sigaction directly. The pqsignal()
// wrapper preserves the PG API and ensures SA_RESTART is set on all
// handlers (matching PG's behaviour for non-blocking sockets).
#pragma once

#include <csignal>

namespace pgcpp::protocol {

// SignalHandler — function-pointer signal handler (PG's pqsigfunc_t).
using SignalHandler = void (*)(int);

// pqsignal — install `handler` for `signo`, with SA_RESTART.
// Returns the previous handler (or SIG_ERR on error).
SignalHandler pqsignal(int signo, SignalHandler handler);

// pqsignal_no_restart — like pqsignal but does NOT set SA_RESTART.
// Used for signals that must interrupt blocking syscalls (SIGALRM, SIGINT).
SignalHandler pqsignal_no_restart(int signo, SignalHandler handler);

// pq_block_sigalrm — block SIGALRM (returns 0 on success, -1 on error).
// Used by PG around calls that must not be interrupted by SIGALRM.
int pq_block_sigalrm();

// pq_reset_sigalrm — unblock SIGALRM (returns 0 on success, -1 on error).
int pq_reset_sigalrm();

// pq_sigprocmask — portable wrapper around pthread_sigmask / sigprocmask.
// `how` is SIG_BLOCK / SIG_UNBLOCK / SIG_SETMASK.
int pq_sigprocmask(int how, const sigset_t* set, sigset_t* oldset);

// GetInstalledHandler — return the currently-installed handler for `signo`
// (or nullptr if none). Used by tests.
SignalHandler GetInstalledHandler(int signo);

// ResetAllSignalHandlers — restore default handlers for all signals we
// might have installed. Used by tests.
void ResetAllSignalHandlers();

}  // namespace pgcpp::protocol

// interrupt.h — Signal/interrupt handling for backend and auxiliary processes.
//
// Converted from PostgreSQL 15's src/backend/tcop/postgres.c (interrupt section)
// and src/backend/storage/ipc/signalfuncs.c.
//
// PostgreSQL uses a deferred-interrupt model: signal handlers only set
// "pending" flags; the main loop calls CHECK_FOR_INTERRUPTS() at safe points
// to process them. This avoids longjmp-from-signal-handler issues and keeps
// the executor's invariants intact.
//
// MyToyDB preserves this model. Signal handlers set atomic flags; the main
// loop calls HandleInterrupts() which dispatches each pending flag to its
// registered handler. C++ exceptions are disabled (per AGENTS.md), so a
// query-cancel interrupt sets a flag that the executor checks via
// InterruptRequested().
#pragma once

#include <atomic>
#include <functional>
#include <string>

namespace mytoydb::server {

// InterruptFlags — atomic flags set by signal handlers.
//
// All members are std::atomic<bool> for signal-safety. Signal handlers
// only call store(...) on these flags; the main thread processes them.
struct InterruptFlags {
    // InterruptPending — generic "some interrupt is pending" flag.
    static std::atomic<bool> InterruptPending;

    // QueryCancelPending — user requested query cancellation (SIGINT in psql).
    static std::atomic<bool> QueryCancelPending;

    // ProcDiePending — backend should die (SIGTERM during query).
    static std::atomic<bool> ProcDiePending;

    // CheckpointTimeout — checkpoint timer expired.
    static std::atomic<bool> CheckpointTimeout;

    // ReloadConfigPending — SIGHUP received, reload configuration.
    static std::atomic<bool> ReloadConfigPending;

    // ShutdownRequested — graceful shutdown requested (SIGTERM to postmaster).
    static std::atomic<bool> ShutdownRequested;

    // BgWriterShutdownRequested — bgwriter-specific shutdown.
    static std::atomic<bool> BgWriterShutdownRequested;

    // WalWriterShutdownRequested — walwriter-specific shutdown.
    static std::atomic<bool> WalWriterShutdownRequested;
};

// Reset all interrupt flags to false (for testing).
void ResetInterruptFlags();

// ClearInterruptHandlers — remove all registered interrupt handlers.
// Used by tests to ensure handler state does not leak between test cases.
void ClearInterruptHandlers();

// --- Signal handlers (signal-safe, only set flags) ---

// HandleQueryCancelSignal — SIGINT handler for backends.
void HandleQueryCancelSignal(int sig);

// HandleProcDieSignal — SIGTERM handler for backends (during query).
void HandleProcDieSignal(int sig);

// HandleReloadConfigSignal — SIGHUP handler.
void HandleReloadConfigSignal(int sig);

// HandleShutdownSignal — SIGTERM handler for postmaster / aux processes.
void HandleShutdownSignal(int sig);

// HandleChildShutdownSignal — SIGQUIT handler (immediate shutdown).
void HandleChildShutdownSignal(int sig);

// InstallBackendSignalHandlers — install the standard signal handlers
// for a forked backend process (SIGINT, SIGTERM, SIGHUP).
void InstallBackendSignalHandlers();

// ResetBackendSignalHandlers — restore default signal handlers.
void ResetBackendSignalHandlers();

// InstallAuxProcessSignalHandlers — install handlers for an auxiliary
// process (only SIGHUP and SIGTERM; no SIGINT — aux processes don't run
// user queries).
void InstallAuxProcessSignalHandlers();

// --- Deferred interrupt processing ---

// HandleInterrupts — process any pending interrupts. Called at safe points
// in the main loop. Dispatches each pending flag to its registered handler.
// Does NOT raise ereport(ERROR); just clears flags and runs handlers.
void HandleInterrupts();

// CheckForInterrupts — convenience wrapper: checks InterruptPending, and
// if set, calls HandleInterrupts. This is the "CHECK_FOR_INTERRUPTS()"
// macro equivalent.
void CheckForInterrupts();

// InterruptRequested — true if a query-cancel or proc-die interrupt is
// pending. Used by the executor to check whether to abort the current query.
bool InterruptRequested();

// WaitForInterrupt — block until any interrupt flag is set, with a timeout
// in milliseconds. Returns true if an interrupt was received, false on
// timeout. (Used by auxiliary processes' main loops.)
bool WaitForInterrupt(int timeout_ms);

// InterruptHandlerFn — handler invoked when a specific interrupt fires.
using InterruptHandlerFn = std::function<void()>;

// RegisterInterruptHandler — register a handler for a named interrupt.
// The handler is called by HandleInterrupts when the corresponding flag
// is set. Returns a handler ID (positive on success, -1 on error).
int RegisterInterruptHandler(const std::string& name, InterruptHandlerFn handler);

// UnregisterInterruptHandler — remove a previously-registered handler.
void UnregisterInterruptHandler(int handler_id);

// SignalInterruptHandler — invoke a named interrupt handler directly.
// Used by tests to simulate interrupt arrival without an actual signal.
void SignalInterruptHandler(const std::string& name);

}  // namespace mytoydb::server

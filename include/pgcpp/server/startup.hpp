// startup.h — Startup process: performs crash recovery on server startup.
//
// Converted from PostgreSQL 15's src/backend/postmaster/startup.c.
//
// The startup process is the first auxiliary process forked by the
// postmaster after a crash or unclean shutdown. It reads the WAL from
// the last checkpoint and replays all records to restore the database
// to a consistent state. Once recovery completes, the postmaster begins
// accepting client connections.
//
// In PostgreSQL, startup is a separate process that communicates with the
// postmaster via shared memory. pgcpp represents it as a stateful API
// that wraps PerformCrashRecovery (from xlogrecovery.h).
#pragma once

#include <cstdint>

#include "pgcpp/transaction/xlog.hpp"

namespace pgcpp::server {

// StartupState — the state of the startup process.
enum class StartupState {
    // kNotStarted — StartupProcessMain has not been called.
    kNotStarted,
    // kInitializing — startup process is initializing subsystems.
    kInitializing,
    // kRecovering — crash recovery is in progress (WAL replay).
    kRecovering,
    // kConsistent — recovery completed; database is in a consistent state.
    kConsistent,
    // kShuttingDown — startup process is shutting down.
    kShuttingDown,
    // kDone — startup process has exited.
    kDone,
};

// StartupStats — statistics tracked by the startup process.
struct StartupStats {
    // Number of WAL records replayed.
    uint64_t records_replayed = 0;
    // Number of WAL records skipped (no registered redo function).
    uint64_t records_skipped = 0;
    // Last RMGR ID processed.
    uint8_t last_rmid = 0;
    // Last record LSN processed.
    uint64_t last_lsn = 0;
    // Recovery duration in milliseconds.
    int64_t recovery_duration_ms = 0;
    // Final LSN reached after recovery (next insert position).
    uint64_t recovery_end_lsn = 0;
};

// InitializeStartupProcess — set up startup process state.
void InitializeStartupProcess();

// ResetStartupProcess — clear startup process state (for testing).
void ResetStartupProcess();

// StartupProcessMain — the main loop of the startup process.
// Performs crash recovery: reads the WAL from the start and replays all
// records via the registered redo functions. Transitions through
// kInitializing → kRecovering → kConsistent → kDone. Returns 0 on
// successful recovery, non-zero on error.
int StartupProcessMain();

// GetStartupState — return the current startup state.
StartupState GetStartupState();

// IsRecoveryInProgress — true if the startup process is still replaying
// WAL (state == kRecovering).
bool IsRecoveryInProgress();

// GetStartupStats — return statistics about the recovery.
StartupStats GetStartupStats();

// SetRecoveryStartLsn — set the LSN where recovery should start.
// Defaults to kSizeofXlogRecord (start of the WAL stream).
void SetRecoveryStartLsn(transaction::XLogRecPtr lsn);

}  // namespace pgcpp::server

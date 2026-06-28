// walwriter.h — WAL writer: flushes WAL buffer to disk periodically.
//
// Converted from PostgreSQL 15's src/backend/postmaster/walwriter.c.
//
// The WAL writer runs in a loop, periodically calling XLogFlush() to
// push the WAL buffer's contents to disk. This ensures that committed
// transactions become durable promptly without backends having to wait
// for fsync at commit time.
//
// In PostgreSQL, the WAL writer is a separate process forked by the
// postmaster. pgcpp represents it as a stateful API (in-memory WAL
// buffer is always "durable", so flushes are no-ops; the API preserves
// the schedule/flush/main-loop structure).
#pragma once

#include <cstdint>

namespace pgcpp::server {

// WalWriterStats — statistics tracked by the WAL writer.
struct WalWriterStats {
    // Number of bytes of WAL flushed to disk.
    uint64_t bytes_written = 0;
    // Number of flush cycles run.
    uint64_t flush_cycles = 0;
    // Last LSN flushed.
    uint64_t last_flush_lsn = 0;
    // Timestamp (ms since epoch) of the last flush cycle.
    int64_t last_flush_time_ms = 0;
    // Whether the WAL writer is currently running its main loop.
    bool running = false;
};

// InitializeWalWriter — set up WAL writer state (clear stats).
void InitializeWalWriter();

// ResetWalWriter — clear WAL writer state and statistics (for testing).
void ResetWalWriter();

// WalWriterScheduleFlush — request the WAL writer to flush WAL.
void WalWriterScheduleFlush();

// WalWriterFlush — flush WAL up to the current insert pointer.
// Returns the number of bytes flushed (may be 0 if no new WAL).
uint64_t WalWriterFlush();

// WalWriterMain — the main loop of the WAL writer (simplified: runs a
// fixed number of iterations, then returns). Returns the number of
// flush cycles performed.
int WalWriterMain(int max_iterations);

// WalWriterShutdown — request the WAL writer to stop (sets running=false).
void WalWriterShutdown();

// WalWriterIsRunning — true if the WAL writer is in its main loop.
bool WalWriterIsRunning();

// GetWalWriterStats — return the current WAL writer statistics.
WalWriterStats GetWalWriterStats();

}  // namespace pgcpp::server

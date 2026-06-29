// bgwriter.h — Background writer: flushes dirty buffers to disk asynchronously.
//
// Converted from PostgreSQL 15's src/backend/postmaster/bgwriter.c.
//
// The bgwriter runs in a loop, periodically scanning the buffer pool for
// dirty pages and writing them to disk. This keeps the pool from filling
// up with dirty pages, so that backends doing queries rarely need to
// perform write I/O themselves (which would block query execution).
//
// In PostgreSQL, bgwriter is a separate process forked by the postmaster.
// pgcpp is single-process, so the bgwriter is a stateful API that can
// be invoked in a loop or scheduled to run periodically (no actual fork
// needed for the simplified model).
#pragma once

#include <cstdint>

namespace pgcpp::server {

// BgWriterStats — statistics tracked by the background writer.
struct BgWriterStats {
    // Number of dirty buffers written to disk.
    uint64_t buffers_written = 0;
    // Number of buffers allocated (newly read into the pool) by the bgwriter.
    uint64_t buffers_alloc = 0;
    // Number of flush cycles run.
    uint64_t flush_cycles = 0;
    // Timestamp (ms since epoch) of the last flush cycle.
    int64_t last_flush_time_ms = 0;
    // Whether the bgwriter is currently running its main loop.
    bool running = false;
};

// InitializeBgWriter — set up bgwriter state (clear stats).
void InitializeBgWriter();

// ResetBgWriter — clear bgwriter state and statistics (for testing).
void ResetBgWriter();

// BgWriterScheduleFlush — request the bgwriter to flush dirty buffers.
// `target_count` is the maximum number of buffers to flush in the next
// cycle. The bgwriter will flush up to this many dirty buffers.
void BgWriterScheduleFlush(int target_count);

// BgWriterFlushBuffers — flush up to `max_buffers` dirty buffers to disk.
// Returns the number of buffers actually flushed. Called by the bgwriter's
// main loop, but can also be called directly (e.g. by tests).
int BgWriterFlushBuffers(int max_buffers);

// BgWriterMain — the main loop of the bgwriter (simplified: runs a fixed
// number of iterations, then returns). Each iteration flushes up to the
// scheduled number of dirty buffers. Returns the total buffers flushed.
int BgWriterMain(int max_iterations);

// BgWriterShutdown — request the bgwriter to stop (sets running=false).
void BgWriterShutdown();

// BgWriterIsRunning — true if the bgwriter is in its main loop.
bool BgWriterIsRunning();

// GetBgWriterStats — return the current bgwriter statistics.
BgWriterStats GetBgWriterStats();

}  // namespace pgcpp::server

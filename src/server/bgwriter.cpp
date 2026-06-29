// bgwriter.cpp — Background writer: flushes dirty buffers to disk.
//
// Converted from PostgreSQL 15's src/backend/postmaster/bgwriter.c.
//
// The bgwriter periodically scans the buffer pool for dirty pages and
// writes them to disk. In pgcpp (single-process, in-memory buffer pool),
// the bgwriter is a stateful API: BgWriterScheduleFlush queues a target
// count of dirty buffers to flush, BgWriterFlushBuffers flushes up to
// that many (returning the count actually flushed), and BgWriterMain runs
// the main loop for a fixed number of iterations.
//
// The "pending dirty count" is a simplified stand-in for PG's buffer pool
// scan: tests can increment it via BgWriterScheduleFlush to simulate
// dirty buffers accumulating, then verify that FlushBuffers drains it.
#include "server/bgwriter.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>

#include "server/interrupt.hpp"

namespace pgcpp::server {

namespace {

BgWriterStats& Stats() {
    static BgWriterStats s;
    return s;
}

// Pending flush target: number of dirty buffers waiting to be flushed.
// Set by BgWriterScheduleFlush, decremented by BgWriterFlushBuffers.
int& PendingFlushTarget() {
    static int target = 0;
    return target;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

void InitializeBgWriter() {
    Stats() = BgWriterStats{};
    PendingFlushTarget() = 0;
}

void ResetBgWriter() {
    InitializeBgWriter();
}

void BgWriterScheduleFlush(int target_count) {
    PendingFlushTarget() = std::max(PendingFlushTarget(), target_count);
}

int BgWriterFlushBuffers(int max_buffers) {
    int& target = PendingFlushTarget();
    int to_flush = std::min(target, max_buffers);
    if (to_flush <= 0) {
        return 0;
    }

    target -= to_flush;
    auto& s = Stats();
    s.buffers_written += static_cast<uint64_t>(to_flush);
    return to_flush;
}

int BgWriterMain(int max_iterations) {
    auto& s = Stats();
    s.running = true;

    int total_flushed = 0;
    for (int i = 0; i < max_iterations; ++i) {
        if (InterruptFlags::ShutdownRequested.load() ||
            InterruptFlags::BgWriterShutdownRequested.load()) {
            break;
        }

        // Flush up to a batch of buffers per iteration (PG default: 256).
        int flushed = BgWriterFlushBuffers(/*max_buffers=*/256);
        total_flushed += flushed;
        ++s.flush_cycles;
        s.last_flush_time_ms = NowMs();

        // If nothing to flush, we'd sleep here in PG; in pgcpp we just exit
        // the loop early to avoid busy-waiting in tests.
        if (flushed == 0) {
            break;
        }
    }

    s.running = false;
    return total_flushed;
}

void BgWriterShutdown() {
    InterruptFlags::BgWriterShutdownRequested = true;
}

bool BgWriterIsRunning() {
    return Stats().running;
}

BgWriterStats GetBgWriterStats() {
    return Stats();
}

}  // namespace pgcpp::server

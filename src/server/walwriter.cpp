// walwriter.cpp — WAL writer: flushes WAL buffer periodically.
//
// Converted from PostgreSQL 15's src/backend/postmaster/walwriter.c.
//
// The WAL writer runs in a loop, periodically calling XLogFlush() to push
// WAL buffer contents to disk. In pgcpp (in-memory WAL buffer, always
// "durable"), XLogFlush is a no-op; the WAL writer tracks the current
// insert position and counts any new bytes as "flushed".
#include "server/walwriter.hpp"

#include <chrono>
#include <cstdint>

#include "server/interrupt.hpp"
#include "transaction/xlog.hpp"

namespace pgcpp::server {

namespace {

WalWriterStats& Stats() {
    static WalWriterStats s;
    return s;
}

// Last LSN we flushed up to. Initialized to 0; on each flush cycle, we
// compare to GetXLogInsertRecPtr() and count the difference as "bytes
// flushed".
uint64_t& LastFlushedLsn() {
    static uint64_t lsn = 0;
    return lsn;
}

// Pending flush flag.
bool& FlushPending() {
    static bool pending = false;
    return pending;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

void InitializeWalWriter() {
    Stats() = WalWriterStats{};
    LastFlushedLsn() = 0;
    FlushPending() = false;
}

void ResetWalWriter() {
    InitializeWalWriter();
}

void WalWriterScheduleFlush() {
    FlushPending() = true;
}

uint64_t WalWriterFlush() {
    uint64_t current_lsn = transaction::GetXLogInsertRecPtr();
    uint64_t& last = LastFlushedLsn();
    uint64_t bytes = (current_lsn > last) ? (current_lsn - last) : 0;

    if (bytes > 0) {
        transaction::XLogFlush(current_lsn);
        last = current_lsn;
    }

    auto& s = Stats();
    s.bytes_written += bytes;
    s.last_flush_lsn = current_lsn;
    s.last_flush_time_ms = NowMs();
    FlushPending() = false;
    return bytes;
}

int WalWriterMain(int max_iterations) {
    auto& s = Stats();
    s.running = true;

    int cycles = 0;
    for (int i = 0; i < max_iterations; ++i) {
        if (InterruptFlags::ShutdownRequested.load() ||
            InterruptFlags::WalWriterShutdownRequested.load()) {
            break;
        }

        WalWriterFlush();
        ++cycles;
        ++s.flush_cycles;

        // If nothing was scheduled and no new WAL arrived, exit early
        // to avoid busy-waiting in tests.
        if (!FlushPending() && s.bytes_written == 0) {
            // Still count one cycle, then stop.
            break;
        }
    }

    s.running = false;
    return cycles;
}

void WalWriterShutdown() {
    InterruptFlags::WalWriterShutdownRequested = true;
}

bool WalWriterIsRunning() {
    return Stats().running;
}

WalWriterStats GetWalWriterStats() {
    return Stats();
}

}  // namespace pgcpp::server

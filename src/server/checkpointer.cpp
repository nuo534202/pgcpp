// checkpointer.cpp — Checkpointer: performs checkpoints.
//
// Converted from PostgreSQL 15's src/backend/postmaster/checkpointer.c.
//
// A checkpoint writes all dirty buffer pool pages to disk and flushes the
// WAL up to the current insert position. RequestCheckpoint queues a
// request (with flags), CheckpointerMain processes pending requests,
// and CreateCheckPoint performs an immediate checkpoint.
//
// In pgcpp, the checkpoint flushes dirty buffers by calling
// BgWriterFlushBuffers (with a large max to drain everything), then
// calls XLogFlush to push WAL up to the current insert LSN. The new
// checkpoint LSN is recorded for crash-recovery restart.
#include "server/checkpointer.hpp"

#include <chrono>
#include <cstdint>

#include "server/bgwriter.hpp"
#include "server/interrupt.hpp"
#include "transaction/xlog.hpp"

namespace pgcpp::server {

namespace {

CheckpointStats& Stats() {
    static CheckpointStats s;
    return s;
}

// Pending checkpoint request flags (0 = no pending request).
uint32_t& PendingRequest() {
    static uint32_t flags = 0;
    return flags;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

void InitializeCheckpointer() {
    Stats() = CheckpointStats{};
    PendingRequest() = 0;
}

void ResetCheckpointer() {
    InitializeCheckpointer();
}

void RequestCheckpoint(uint32_t flags) {
    PendingRequest() |= flags;
}

bool CreateCheckPoint(uint32_t flags) {
    auto& s = Stats();

    if (flags & kCheckpointCauseTime) {
        ++s.checkpoints_timed;
    } else {
        ++s.checkpoints_requested;
    }

    // Flush all dirty buffers to disk via the bgwriter's flush path.
    // PG's checkpointer calls SyncOneBuffer / BufferSync; pgcpp reuses
    // BgWriterFlushBuffers with a large max to drain the pending target.
    BgWriterScheduleFlush(/*target_count=*/1000000);
    int flushed = BgWriterFlushBuffers(/*max_buffers=*/1000000);
    s.buffers_written += static_cast<uint64_t>(flushed);

    // Flush WAL up to the current insert position.
    uint64_t current_lsn = transaction::GetXLogInsertRecPtr();
    transaction::XLogFlush(current_lsn);

    s.last_checkpoint_lsn = current_lsn;
    s.last_checkpoint_time_ms = NowMs();
    return true;
}

int CheckpointerMain(int max_iterations) {
    auto& s = Stats();
    s.running = true;

    int checkpoints_done = 0;
    for (int i = 0; i < max_iterations; ++i) {
        if (InterruptFlags::ShutdownRequested.load()) {
            break;
        }

        uint32_t pending = PendingRequest();
        if (pending == 0) {
            // No pending request: exit early (in PG, we'd sleep).
            break;
        }

        PendingRequest() = 0;
        CreateCheckPoint(pending);
        ++checkpoints_done;
    }

    s.running = false;
    return checkpoints_done;
}

void CheckpointerShutdown() {
    InterruptFlags::ShutdownRequested = true;
}

bool CheckpointerIsRunning() {
    return Stats().running;
}

uint64_t LastCheckpointLSN() {
    return Stats().last_checkpoint_lsn;
}

CheckpointStats GetCheckpointStats() {
    return Stats();
}

}  // namespace pgcpp::server

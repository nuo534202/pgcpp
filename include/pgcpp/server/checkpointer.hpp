// checkpointer.h — Checkpointer: performs checkpoints by flushing all dirty
// buffers and WAL to disk.
//
// Converted from PostgreSQL 15's src/backend/postmaster/checkpointer.c.
//
// A checkpoint writes all dirty buffer pool pages to disk and flushes the
// WAL up to the current insert position. PostgreSQL uses checkpoints as
// crash-recovery restart points: on startup, only WAL records after the
// last checkpoint need to be replayed.
//
// In PostgreSQL, the checkpointer is a separate process. MyToyDB represents
// it as a stateful API: RequestCheckpoint queues a request, CheckpointerMain
// processes pending requests, and CreateCheckPoint performs an immediate
// checkpoint.
#pragma once

#include <cstdint>

namespace mytoydb::server {

// CheckpointFlags — flags controlling checkpoint behavior.
// Matches PostgreSQL's CHECKPOINT_* constants.
enum CheckpointFlags : uint32_t {
    kCheckpointNone = 0,
    // CHECKPOINT_IS_SHUTDOWN — checkpoint at server shutdown.
    kCheckpointIsShutdown = 1u << 0,
    // CHECKPOINT_END_OF_RECOVERY — checkpoint at end of crash recovery.
    kCheckpointEndOfRecovery = 1u << 1,
    // CHECKPOINT_IMMEDIATE — perform checkpoint without delay.
    kCheckpointImmediate = 1u << 2,
    // CHECKPOINT_FORCE — force a checkpoint even if none needed.
    kCheckpointForce = 1u << 3,
    // CHECKPOINT_WAIT — wait for the checkpoint to complete.
    kCheckpointWait = 1u << 4,
    // CHECKPOINT_CAUSE_XLOG — checkpoint triggered by WAL consumption.
    kCheckpointCauseXlog = 1u << 5,
    // CHECKPOINT_CAUSE_TIME — checkpoint triggered by timeout.
    kCheckpointCauseTime = 1u << 6,
};

// CheckpointStats — statistics tracked by the checkpointer.
struct CheckpointStats {
    // Number of requested checkpoints (RequestCheckpoint calls).
    uint64_t checkpoints_requested = 0;
    // Number of timed checkpoints (auto-triggered by checkpoint_timeout).
    uint64_t checkpoints_timed = 0;
    // Number of buffers written by checkpoints.
    uint64_t buffers_written = 0;
    // Last checkpoint LSN (byte offset in WAL).
    uint64_t last_checkpoint_lsn = 0;
    // Timestamp (ms since epoch) of the last checkpoint.
    int64_t last_checkpoint_time_ms = 0;
    // Whether the checkpointer is currently running its main loop.
    bool running = false;
};

// InitializeCheckpointer — set up checkpointer state (clear stats).
void InitializeCheckpointer();

// ResetCheckpointer — clear checkpointer state and statistics (for testing).
void ResetCheckpointer();

// RequestCheckpoint — request the checkpointer to perform a checkpoint.
// If kCheckpointWait is set, blocks until the checkpoint completes.
void RequestCheckpoint(uint32_t flags);

// CreateCheckPoint — perform a checkpoint immediately.
// Flushes all dirty buffers and WAL to disk, records the current LSN as
// the new checkpoint LSN. Returns true on success.
bool CreateCheckPoint(uint32_t flags);

// CheckpointerMain — the main loop of the checkpointer (simplified: runs
// a fixed number of iterations processing pending requests). Returns the
// number of checkpoints performed.
int CheckpointerMain(int max_iterations);

// CheckpointerShutdown — request the checkpointer to stop (sets running=false).
void CheckpointerShutdown();

// CheckpointerIsRunning — true if the checkpointer is in its main loop.
bool CheckpointerIsRunning();

// LastCheckpointLSN — return the LSN of the last checkpoint (0 if none).
uint64_t LastCheckpointLSN();

// GetCheckpointStats — return the current checkpointer statistics.
CheckpointStats GetCheckpointStats();

}  // namespace mytoydb::server

// sync.h — Pending fsync queue (checkpointer integration).
//
// Converted from PostgreSQL 15's src/include/storage/sync.h and
// src/backend/storage/sync/sync.c.
//
// PostgreSQL batches fsync requests from backends into a per-segment queue
// processed by the checkpointer. The checkpointer then fsyncs each segment
// in a single pass before recording the next checkpoint LSN.
//
// pgcpp is single-process, so the queue is in-memory. The API mirrors
// PG's RegisterSyncRequest / ProcessSyncRequests surface.
#pragma once

#include <cstdint>
#include <vector>

#include "storage/relfilenode.hpp"

namespace pgcpp::storage {

// ForkNumber — re-exported from relfilenode for callers of the sync API.
// (Already declared in relfilenode.hpp; we re-export it here for ergonomic
// include-only-sync callers.)
using ForkNumberReexport = int;

// SyncRequestTag — identifies one fsync target (relation fork segment).
struct SyncRequestTag {
    RelFileNode rnode;
    int fork_num = 0;    // ForkNumber (Main=0, Fsm, VisibilityMap, Init)
    uint64_t segno = 0;  // segment number (1-based; 0 = whole relation)

    bool operator==(const SyncRequestTag&) const = default;
};

// SyncRequestHash — hash for use in std::unordered_map.
struct SyncRequestTagHash {
    std::size_t operator()(const SyncRequestTag& t) const {
        return std::hash<uint32_t>()(t.rnode.spc_node) ^
               (std::hash<uint32_t>()(t.rnode.db_node) << 1) ^
               (std::hash<uint32_t>()(t.rnode.rel_node) << 2) ^
               (std::hash<int>()(t.fork_num) << 3) ^ (std::hash<uint64_t>()(t.segno) << 4);
    }
};

// SyncRequest — one pending fsync request.
struct SyncRequest {
    SyncRequestTag tag;
    bool completed = false;
};

// RegisterSyncRequest — enqueue a fsync request for the checkpointer.
// Returns true on enqueue success, false if the queue is full (caller should
// then sync inline).
bool RegisterSyncRequest(const SyncRequestTag& tag);

// ProcessSyncRequests — fsync all pending requests and clear the queue.
// Returns the number of requests processed.
int ProcessSyncRequests();

// SyncFile — synchronously fsync a single file (PG's mdsyncfiletag).
// In pgcpp this is a no-op that records the call (since the underlying
// smgr/md layer handles fsync at file close time for the in-memory model).
// Returns 0 on success, -1 on error.
int SyncFile(const SyncRequestTag& tag);

// NumPendingSyncRequests — count of currently-queued requests.
int NumPendingSyncRequests();

// GetPendingSyncRequests — snapshot of the current queue (for tests).
std::vector<SyncRequest> GetPendingSyncRequests();

// ResetSyncQueue — drop all pending requests (used by tests).
void ResetSyncQueue();

// GetSyncStats — return (total_requests, total_processed, total_errors).
struct SyncStats {
    int64_t total_requests = 0;
    int64_t total_processed = 0;
    int64_t total_errors = 0;
};
SyncStats GetSyncStats();

}  // namespace pgcpp::storage

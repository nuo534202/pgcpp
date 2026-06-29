// sync.cpp — Pending fsync queue (checkpointer integration).
//
// Converted from PostgreSQL 15's src/backend/storage/sync/sync.c.
#include "storage/sync/sync.hpp"

#include <unordered_map>

namespace pgcpp::storage {

namespace {

// PendingMap — dedup queue of pending fsync requests keyed by SyncRequestTag.
// We use a map so duplicate requests collapse to a single entry (PG's
// hash-dedup behavior).
std::unordered_map<SyncRequestTag, SyncRequest, SyncRequestTagHash>& Pending() {
    static std::unordered_map<SyncRequestTag, SyncRequest, SyncRequestTagHash> p;
    return p;
}

SyncStats& Stats() {
    static SyncStats s;
    return s;
}

}  // namespace

bool RegisterSyncRequest(const SyncRequestTag& tag) {
    if (Pending().find(tag) != Pending().end()) {
        // Already queued.
        return true;
    }
    SyncRequest req;
    req.tag = tag;
    req.completed = false;
    Pending()[tag] = req;
    Stats().total_requests++;
    return true;
}

int ProcessSyncRequests() {
    int processed = 0;
    auto& pending = Pending();
    for (auto& kv : pending) {
        if (!kv.second.completed) {
            int rc = SyncFile(kv.second.tag);
            if (rc == 0) {
                kv.second.completed = true;
                processed++;
                Stats().total_processed++;
            } else {
                Stats().total_errors++;
            }
        }
    }
    // Drop completed entries.
    for (auto it = pending.begin(); it != pending.end();) {
        if (it->second.completed) {
            it = pending.erase(it);
        } else {
            ++it;
        }
    }
    return processed;
}

int SyncFile(const SyncRequestTag& tag) {
    // In pgcpp the underlying smgr/md layer does fsync at file close time,
    // so a per-segment fsync is effectively a no-op. We honor the call by
    // returning 0 (success) without doing anything — the in-memory storage
    // model never leaves data unsynced.
    (void)tag;
    return 0;
}

int NumPendingSyncRequests() {
    return static_cast<int>(Pending().size());
}

std::vector<SyncRequest> GetPendingSyncRequests() {
    std::vector<SyncRequest> result;
    for (const auto& kv : Pending()) {
        result.push_back(kv.second);
    }
    return result;
}

void ResetSyncQueue() {
    Pending().clear();
    Stats() = SyncStats{};
}

SyncStats GetSyncStats() {
    return Stats();
}

}  // namespace pgcpp::storage

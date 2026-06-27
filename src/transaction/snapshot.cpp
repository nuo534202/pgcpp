// snapshot.cpp — Snapshot management for MVCC.
//
// Converted from PostgreSQL 15's src/backend/utils/time/snapmgr.cpp and
// src/backend/storage/ipc/procarray.cpp (GetSnapshotData).
//
// In PostgreSQL, GetSnapshotData scans the ProcArray (the array of all
// backend processes) to find in-progress transactions. In MyToyDB
// (single-process), there is only one transaction active at a time, so
// the snapshot is simpler:
//   - xmin = oldest XID that might still be in progress (typically the
//     current transaction's XID, or FirstNormalTransactionId if idle)
//   - xmax = next XID to be assigned (XIDs >= xmax don't exist yet)
//   - xip = empty (no concurrent transactions in single-process mode)
//
// The current transaction sees its own changes via the command ID (curcid),
// not via the xip list. A tuple inserted by the current transaction is
// visible if its t_cid <= snapshot.curcid.
#include "mytoydb/transaction/snapshot.hpp"

#include <vector>

#include "mytoydb/common/containers/node.hpp"
#include "mytoydb/common/memory/memory_context.hpp"
#include "mytoydb/transaction/transam.hpp"
#include "mytoydb/transaction/xact.hpp"

namespace mytoydb::transaction {
using mytoydb::nodes::makePallocNode;

namespace {

// The active snapshot stack. The top (back) is the current active snapshot.
// Implemented as a function-local static to avoid static-initialization-order
// issues (matches the CommitLog() pattern in transam.cpp).
std::vector<Snapshot>& ActiveSnapshotStack() {
    static std::vector<Snapshot> stack;
    return stack;
}

// The cached transaction snapshot (pushed onto the stack on first use of
// GetTransactionSnapshot). Kept for backward compatibility with the original
// single-snapshot implementation.
SnapshotData* active_snapshot = nullptr;

// The cached CatalogSnapshot (lazily built, invalidated on catalog change or
// transaction end). Memory is owned by the allocating memory context.
Snapshot catalog_snapshot = nullptr;

}  // namespace

void GetSnapshotData(SnapshotData* snapshot) {
    if (snapshot == nullptr)
        return;

    snapshot->snapshot_type = SnapshotType::kMVCC;

    // The current transaction's XID (if any).
    TransactionId current_xid = GetCurrentTransactionIdIfAny();

    // xmax = the next XID to be assigned (XIDs >= xmax don't exist yet).
    snapshot->xmax = GetNextTransactionId() + 1;

    // xmin = the oldest XID that might still be in progress.
    // In single-process mode, this is either the current XID (if a
    // transaction is active) or the next XID (if idle).
    if (TransactionIdIsValid(current_xid)) {
        snapshot->xmin = current_xid;
    } else {
        // No active transaction — all existing XIDs are finished.
        snapshot->xmin = snapshot->xmax;
    }

    // xip = list of in-progress XIDs in [xmin, xmax).
    // In single-process mode, the only in-progress transaction is the
    // current one, which is handled via curcid (not xip). So xip is empty.
    snapshot->xip.clear();

    // Record the current transaction's XID and command ID.
    snapshot->snapshot_xid = current_xid;
    snapshot->curcid = GetCurrentCommandId(false);

    snapshot->taken_during_recovery = false;
}

Snapshot GetLatestSnapshot() {
    auto* snap = makePallocNode<SnapshotData>();
    GetSnapshotData(snap);
    return snap;
}

Snapshot GetTransactionSnapshot() {
    // If the stack already has an active snapshot, return its top.
    if (!ActiveSnapshotStack().empty()) {
        return ActiveSnapshotStack().back();
    }

    // Create a new snapshot for this transaction, cache it, and push it
    // onto the stack so subsequent calls reuse it.
    active_snapshot = GetLatestSnapshot();
    ActiveSnapshotStack().push_back(active_snapshot);
    return active_snapshot;
}

void ResetTransactionSnapshot() {
    // Clear the entire stack and the cached transaction snapshot. The
    // snapshot memory is owned by the memory context it was allocated in
    // (palloc), so we only drop our references here.
    ActiveSnapshotStack().clear();
    active_snapshot = nullptr;
    // CatalogSnapshot is invalidated at transaction end (PG semantics).
    catalog_snapshot = nullptr;
}

void PushActiveSnapshot(Snapshot snapshot) {
    ActiveSnapshotStack().push_back(snapshot);
}

void PushCopiedSnapshot(Snapshot snapshot) {
    Snapshot copy = makePallocNode<SnapshotData>();
    *copy = *snapshot;  // deep-copies fields (including the xip vector)
    ActiveSnapshotStack().push_back(copy);
}

void PopActiveSnapshot() {
    if (!ActiveSnapshotStack().empty()) {
        ActiveSnapshotStack().pop_back();
    }
}

bool ActiveSnapshotSet() {
    return !ActiveSnapshotStack().empty();
}

Snapshot GetActiveSnapshot() {
    if (ActiveSnapshotStack().empty()) {
        return nullptr;
    }
    return ActiveSnapshotStack().back();
}

Snapshot GetCatalogSnapshot() {
    if (catalog_snapshot == nullptr) {
        catalog_snapshot = GetLatestSnapshot();
    }
    return catalog_snapshot;
}

void InvalidateCatalogSnapshot() {
    catalog_snapshot = nullptr;
}

Snapshot GetNonHistoricCatalogSnapshot() {
    if (catalog_snapshot != nullptr) {
        return catalog_snapshot;
    }
    return GetActiveSnapshot();
}

void RegisterSnapshot(Snapshot /*snapshot*/) {
    // No-op in MyToyDB (snapshots are managed by the caller).
}

void UnregisterSnapshot(Snapshot /*snapshot*/) {
    // No-op in MyToyDB.
}

void InitializeSnapshotManager() {
    ActiveSnapshotStack().clear();
    active_snapshot = nullptr;
    catalog_snapshot = nullptr;
}

SnapshotData MakeSnapshot(TransactionId xmin, TransactionId xmax,
                          const std::vector<TransactionId>& xip) {
    SnapshotData snap;
    snap.snapshot_type = SnapshotType::kMVCC;
    snap.xmin = xmin;
    snap.xmax = xmax;
    snap.xip = xip;
    snap.snapshot_xid = GetCurrentTransactionIdIfAny();
    snap.curcid = GetCurrentCommandId(false);
    return snap;
}

}  // namespace mytoydb::transaction

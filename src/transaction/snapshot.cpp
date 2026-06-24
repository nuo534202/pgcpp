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
#include "mytoydb/transaction/snapshot.h"

#include "mytoydb/common/memory/memory_context.h"
#include "mytoydb/transaction/transam.h"
#include "mytoydb/transaction/xact.h"

namespace mytoydb::transaction {

namespace {

// The active snapshot for the current transaction (if any).
SnapshotData* active_snapshot = nullptr;

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
    // Allocate in the current memory context.
    auto* snap = static_cast<SnapshotData*>(mytoydb::memory::palloc(sizeof(SnapshotData)));
    new (snap) SnapshotData();
    GetSnapshotData(snap);
    return snap;
}

Snapshot GetTransactionSnapshot() {
    // If we already have an active snapshot, reuse it.
    if (active_snapshot != nullptr) {
        return active_snapshot;
    }

    // Create a new snapshot for this transaction.
    active_snapshot = GetLatestSnapshot();
    return active_snapshot;
}

void RegisterSnapshot(Snapshot /*snapshot*/) {
    // No-op in MyToyDB (snapshots are managed by the caller).
}

void UnregisterSnapshot(Snapshot /*snapshot*/) {
    // No-op in MyToyDB.
}

void InitializeSnapshotManager() {
    active_snapshot = nullptr;
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

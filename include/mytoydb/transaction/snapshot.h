// snapshot.h — Snapshot data for MVCC visibility checks.
//
// Converted from PostgreSQL 15's src/include/utils/snapshot.h.
//
// A snapshot captures the state of the transaction ID space at a point in
// time, determining which tuple versions are visible to a query.
//
// SnapshotData fields:
//   xmin — all XIDs < xmin are committed (or aborted) and thus "finished".
//          A tuple with t_xmin < snapshot.xmin is visible if committed.
//   xmax — the first XID not yet assigned when the snapshot was taken.
//          A tuple with t_xmin >= xmax didn't exist when the snapshot was
//          taken, so it's invisible.
//   xip  — list of XIDs that were in-progress (not committed) when the
//          snapshot was taken. These are between xmin and xmax. A tuple
//          with t_xmin in xip is invisible (the inserting transaction
//          hadn't committed).
//   curcid — command ID of the snapshot. A tuple inserted by the current
//            transaction is visible only if its t_cid <= curcid.
//   snapshot_xid — XID of the transaction that took this snapshot.
//
// Visibility rule (simplified):
//   A tuple is visible if:
//     1. t_xmin is committed AND (t_xmin < snapshot.xmin OR t_xmin not in xip)
//     2. AND (t_xmax is invalid OR t_xmax is aborted OR
//            (t_xmax is the current transaction AND t_cid > curcid))
#pragma once

#include <cstdint>
#include <vector>

#include "mytoydb/transaction/transam.h"
#include "mytoydb/transaction/xact.h"

namespace mytoydb::transaction {

// SnapshotType — the kind of snapshot (PostgreSQL's SnapshotType).
enum class SnapshotType {
    kMVCC,   // normal MVCC snapshot
    kSelf,   // see only my own changes (for CREATE INDEX CONCURRENTLY)
    kAny,    // see everything (for VACUUM)
    kToast,  // special snapshot for TOAST table access
};

// SnapshotData — the snapshot structure used by visibility checks.
struct SnapshotData {
    SnapshotType snapshot_type = SnapshotType::kMVCC;

    // The XID range of "finished" vs. "in-progress" transactions.
    TransactionId xmin = kInvalidTransactionId;  // all XIDs < xmin are finished
    TransactionId xmax = kInvalidTransactionId;  // XIDs >= xmax didn't exist

    // List of in-progress XIDs in [xmin, xmax) range.
    // These transactions were active when the snapshot was taken.
    std::vector<TransactionId> xip;

    // The XID and command ID of the snapshot's owning transaction.
    TransactionId snapshot_xid = kInvalidTransactionId;
    CommandId curcid = kFirstCommandId;

    // True if this snapshot was taken within a transaction block
    // (affects whether we can see our own changes).
    bool taken_during_recovery = false;

    // --- Convenience predicates ---

    // XidInSnapshot — true if `xid` was in-progress when this snapshot was
    // taken (i.e., xid is in the xip list).
    bool XidInSnapshot(TransactionId xid) const {
        for (TransactionId x : xip) {
            if (x == xid)
                return true;
        }
        return false;
    }

    // XidLtXmin — true if `xid` is older than xmin (definitely finished).
    bool XidLtXmin(TransactionId xid) const {
        return TransactionIdIsValid(xmin) && TransactionIdPrecedes(xid, xmin);
    }

    // XidGexmax — true if `xid` is >= xmax (didn't exist at snapshot time).
    bool XidGeXmax(TransactionId xid) const {
        return TransactionIdIsValid(xmax) && TransactionIdFollowsOrEquals(xid, xmax);
    }
};

// Snapshot — pointer to a SnapshotData.
using Snapshot = SnapshotData*;

// --- Snapshot management API ---

// GetSnapshotData — compute a snapshot for the current transaction.
//
// Scans the transaction state to determine which XIDs are in-progress.
// In MyToyDB (single-process), the only in-progress transaction is the
// current one, so xip is typically empty (the current transaction sees
// its own changes via curcid, not via the xip list).
//
// The snapshot is stored in the provided SnapshotData struct.
void GetSnapshotData(SnapshotData* snapshot);

// GetLatestSnapshot — return a fresh snapshot for the current transaction.
// The snapshot is allocated in the current memory context.
Snapshot GetLatestSnapshot();

// GetTransactionSnapshot — return the snapshot for the current transaction.
// If a snapshot is already registered for this transaction, reuses it;
// otherwise creates a new one.
Snapshot GetTransactionSnapshot();

// ResetTransactionSnapshot — clear the cached transaction snapshot.
//
// Must be called when a transaction commits or aborts so that the next
// transaction gets a fresh snapshot reflecting the latest committed state.
// In PostgreSQL this is handled by AtCommit_Snapshot / AtAbort_Snapshot.
void ResetTransactionSnapshot();

// RegisterSnapshot — register a snapshot for resource management.
// In MyToyDB, this is a no-op (snapshots are managed by the caller).
void RegisterSnapshot(Snapshot snapshot);

// UnregisterSnapshot — release a registered snapshot.
void UnregisterSnapshot(Snapshot snapshot);

// InitializeSnapshotManager — set up the snapshot subsystem.
void InitializeSnapshotManager();

// --- Snapshot construction helpers (for testing) ---

// MakeSnapshot — construct a snapshot with the given parameters.
// Used by tests to create specific visibility scenarios.
SnapshotData MakeSnapshot(TransactionId xmin, TransactionId xmax,
                          const std::vector<TransactionId>& xip = {});

}  // namespace mytoydb::transaction

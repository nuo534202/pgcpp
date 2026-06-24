// visibility.cpp — MVCC visibility check implementation.
//
// Converted from PostgreSQL 15's src/backend/access/heap/heapam_visibility.c.
//
// Implements HeapTupleSatisfiesMVCC, the core visibility predicate that
// determines whether a tuple version is visible to a query.
//
// The algorithm:
//   1. Resolve t_xmin status (committed / aborted / in-progress / current).
//   2. If t_xmin is not visible, the tuple is invisible.
//   3. Resolve t_xmax status (committed / aborted / in-progress / current).
//   4. If t_xmax is visible (committed before snapshot), the tuple is
//      invisible (it was deleted). Otherwise it's visible.
//   5. Set hint flags to accelerate future checks.
#include "mytoydb/transaction/visibility.h"

#include "mytoydb/transaction/transam.h"
#include "mytoydb/transaction/xact.h"

namespace mytoydb::transaction {

bool XidVisibleInSnapshot(TransactionId xid, const SnapshotData& snapshot, bool* hint_committed) {
    if (hint_committed != nullptr)
        *hint_committed = false;

    // Special XIDs (bootstrap, frozen) are always considered committed.
    if (!TransactionIdIsNormal(xid)) {
        if (hint_committed != nullptr)
            *hint_committed = true;
        return true;
    }

    // XIDs >= xmax didn't exist when the snapshot was taken.
    if (snapshot.XidGeXmax(xid)) {
        return false;
    }

    // XIDs < xmin are definitely finished (committed or aborted).
    if (snapshot.XidLtXmin(xid)) {
        // Look up the commit log to determine the final status.
        if (TransactionIdDidCommit(xid)) {
            if (hint_committed != nullptr)
                *hint_committed = true;
            return true;
        }
        return false;  // aborted
    }

    // XID is in [xmin, xmax) — check if it was in-progress at snapshot time.
    if (snapshot.XidInSnapshot(xid)) {
        return false;  // was in-progress, not visible
    }

    // XID is in [xmin, xmax) and not in xip — it was committed.
    if (TransactionIdDidCommit(xid)) {
        if (hint_committed != nullptr)
            *hint_committed = true;
        return true;
    }

    // XID is in [xmin, xmax), not in xip, but not committed — must be aborted.
    return false;
}

bool HeapTupleSatisfiesMVCC(HeapTupleHeaderData* tup, const SnapshotData& snapshot) {
    if (tup == nullptr)
        return false;

    TransactionId xmin = HeapTupleHeaderGetXmin(tup);
    TransactionId xmax = HeapTupleHeaderGetXmax(tup);

    // --- Step 1: Check t_xmin (the inserting transaction) ---

    // Check hint flags first (fast path).
    XactStatus xmin_status = HeapTupleHeaderGetXminStatus(tup);

    bool xmin_visible = false;

    switch (xmin_status) {
        case XactStatus::kCommitted:
            // Hint says committed — but we still need to check the snapshot.
            xmin_visible = true;
            break;

        case XactStatus::kAborted:
            // Hint says aborted — tuple is invisible.
            return false;

        case XactStatus::kFrozen:
            // Frozen XID — always visible (old tuple that was vacuumed).
            xmin_visible = true;
            break;

        case XactStatus::kInProgress:
            // No hint — need to check the commit log and snapshot.
            if (!TransactionIdIsValid(xmin)) {
                // Corrupt tuple — treat as invisible.
                return false;
            }

            // Is this the current transaction?
            if (TransactionIdIsCurrentTransactionId(xmin)) {
                // Current transaction — visible if command ID <= snapshot's.
                uint32_t cid = HeapTupleHeaderGetCid(tup);
                if (cid > snapshot.curcid) {
                    // Inserted by a later command — not visible yet.
                    return false;
                }
                // Check if this is an updated version that was created
                // after the snapshot (HEAP_UPDATED + t_xmin == t_xmax).
                if ((tup->t_infomask & kHeapUpdated) != 0 && xmin == xmax) {
                    return false;
                }
                xmin_visible = true;
            } else {
                // Not the current transaction — check commit log + snapshot.
                bool hint_committed = false;
                xmin_visible = XidVisibleInSnapshot(xmin, snapshot, &hint_committed);

                // Set hint flags if we determined the final status.
                if (hint_committed) {
                    HeapTupleHeaderSetXminCommitted(tup);
                } else if (!xmin_visible) {
                    // If the XID is aborted, set the invalid hint.
                    // But only if we're sure it's aborted (not just in-progress).
                    if (TransactionIdDidAbort(xmin)) {
                        HeapTupleHeaderSetXminInvalid(tup);
                    }
                }
            }
            break;
    }

    // If t_xmin is not visible, the tuple is invisible.
    if (!xmin_visible) {
        return false;
    }

    // --- Step 2: Check t_xmax (the deleting/updating transaction) ---

    // If t_xmax is invalid, the tuple is not deleted — visible.
    if (!TransactionIdIsValid(xmax)) {
        return true;
    }

    // Check hint flags for t_xmax.
    XactStatus xmax_status = HeapTupleHeaderGetXmaxStatus(tup);

    switch (xmax_status) {
        case XactStatus::kCommitted:
            // Hint says committed — check if it was committed before snapshot.
            // If the hint is set, the transaction definitely committed.
            // We need to check the snapshot to see if it was before or after.
            if (snapshot.XidLtXmin(xmax)) {
                // Deleted before snapshot — not visible.
                return false;
            }
            if (snapshot.XidGeXmax(xmax)) {
                // Deletion happened after snapshot — visible.
                return true;
            }
            if (snapshot.XidInSnapshot(xmax)) {
                // Deletion was in-progress at snapshot time — visible
                // (the delete hadn't committed yet).
                return true;
            }
            // xmax is committed and in [xmin, xmax) but not in xip —
            // it was committed before the snapshot. Not visible.
            return false;

        case XactStatus::kAborted:
            // Hint says aborted — deletion didn't happen. Visible.
            return true;

        case XactStatus::kInProgress:
            // No hint — check the commit log and snapshot.
            // Is this the current transaction?
            if (TransactionIdIsCurrentTransactionId(xmax)) {
                // Current transaction — check command ID.
                uint32_t cid = HeapTupleHeaderGetCid(tup);
                // Note: for t_xmax, the CID is the command that deleted the tuple.
                // If cid > snapshot.curcid, the delete happened after the snapshot.
                if (cid > snapshot.curcid) {
                    // Deleted by a later command — still visible.
                    return true;
                }
                // Deleted by an earlier command — not visible.
                return false;
            }

            // Not the current transaction — check commit log + snapshot.
            if (snapshot.XidLtXmin(xmax)) {
                // Deleted before snapshot — check if committed.
                if (TransactionIdDidCommit(xmax)) {
                    HeapTupleHeaderSetXmaxCommitted(tup);
                    return false;  // deleted, not visible
                }
                // Aborted — visible.
                HeapTupleHeaderSetXmaxInvalid(tup);
                return true;
            }

            if (snapshot.XidGeXmax(xmax)) {
                // Deletion happened after snapshot — visible.
                return true;
            }

            if (snapshot.XidInSnapshot(xmax)) {
                // Deletion was in-progress at snapshot time — visible
                // (the delete hadn't committed yet).
                // Check if it eventually committed or aborted.
                if (TransactionIdDidCommit(xmax)) {
                    // It committed, but was in-progress at snapshot time —
                    // the deletion is not visible to this snapshot.
                    return true;
                }
                // It aborted — visible.
                HeapTupleHeaderSetXmaxInvalid(tup);
                return true;
            }

            // xmax is in [xmin, xmax) and not in xip — it was committed
            // before the snapshot. Not visible.
            if (TransactionIdDidCommit(xmax)) {
                HeapTupleHeaderSetXmaxCommitted(tup);
                return false;
            }
            // Aborted — visible.
            HeapTupleHeaderSetXmaxInvalid(tup);
            return true;

        case XactStatus::kFrozen:
            // t_xmax should never be frozen. Treat as not-deleted.
            return true;
    }

    return true;  // unreachable, but satisfies the compiler
}

bool HeapTupleSatisfiesSelf(HeapTupleHeaderData* tup) {
    if (tup == nullptr)
        return false;

    TransactionId xmin = HeapTupleHeaderGetXmin(tup);
    TransactionId xmax = HeapTupleHeaderGetXmax(tup);

    // Check t_xmin
    if (!TransactionIdIsValid(xmin))
        return false;

    if (TransactionIdIsCurrentTransactionId(xmin)) {
        uint32_t cid = HeapTupleHeaderGetCid(tup);
        if (cid > GetCurrentCommandId(false))
            return false;
    } else if (!TransactionIdDidCommit(xmin)) {
        return false;
    }

    // Check t_xmax
    if (!TransactionIdIsValid(xmax))
        return true;

    if (TransactionIdIsCurrentTransactionId(xmax)) {
        uint32_t cid = HeapTupleHeaderGetCid(tup);
        if (cid > GetCurrentCommandId(false))
            return true;
        return false;
    }

    if (!TransactionIdDidCommit(xmax))
        return true;

    return false;
}

bool HeapTupleSatisfiesAny(HeapTupleHeaderData* /*tup*/) {
    // VACUUM sees everything.
    return true;
}

bool HeapTupleIsSurelyDead(const HeapTupleHeaderData* tup, const SnapshotData& snapshot) {
    if (tup == nullptr)
        return false;

    TransactionId xmax = HeapTupleHeaderGetXmax(tup);

    // Not deleted — not dead.
    if (!TransactionIdIsValid(xmax))
        return false;

    // If xmax is not committed, the tuple is not surely dead.
    if (!TransactionIdDidCommit(xmax))
        return false;

    // If xmax >= snapshot.xmin, the deletion might not be visible to all
    // active transactions — can't reclaim yet.
    if (!TransactionIdPrecedes(xmax, snapshot.xmin))
        return false;

    // xmax is committed and older than snapshot.xmin — surely dead.
    return true;
}

}  // namespace mytoydb::transaction

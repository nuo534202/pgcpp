// visibility.h — MVCC visibility checks for heap tuples.
//
// Converted from PostgreSQL 15's src/backend/access/heap/heapam_visibility.c.
//
// HeapTupleSatisfiesMVCC determines whether a tuple version is visible to
// a query running with a given snapshot. The visibility rules implement
// PostgreSQL's MVCC (Multi-Version Concurrency Control):
//
//   A tuple is visible if:
//     1. The inserting transaction (t_xmin) had committed before the
//        snapshot was taken (or is the current transaction with a
//        sufficiently old command ID).
//     2. AND the deleting transaction (t_xmax) either:
//        - Is invalid (tuple not deleted), or
//        - Had not committed before the snapshot was taken, or
//        - Is the current transaction but with a command ID after the
//          snapshot's command ID.
//
// As a side effect, the function sets hint flags on the tuple header
// (HEAP_XMIN_COMMITTED, HEAP_XMIN_INVALID, etc.) to accelerate future
// visibility checks for the same tuple. These hints are not WAL-logged
// and may be lost on crash, but are always safe to recompute.
#pragma once

#include "transaction/heap_tuple.hpp"
#include "transaction/snapshot.hpp"
#include "transaction/transam.hpp"

namespace pgcpp::transaction {

// HeapTupleSatisfiesMVCC — return true if the tuple is visible to the
// given snapshot.
//
// Parameters:
//   tup    — the tuple to check (header is read and hint flags may be set)
//   snapshot — the MVCC snapshot determining visibility
//
// Returns true if the tuple is visible, false otherwise.
//
// Side effects:
//   - May set HEAP_XMIN_COMMITTED / HEAP_XMIN_INVALID hint flags
//   - May set HEAP_XMAX_COMMITTED / HEAP_XMAX_INVALID hint flags
bool HeapTupleSatisfiesMVCC(HeapTupleHeaderData* tup, const SnapshotData& snapshot);

// HeapTupleSatisfiesSelf — return true if the tuple is visible to the
// current transaction (ignoring snapshot; used by CREATE INDEX CONCURRENTLY).
bool HeapTupleSatisfiesSelf(HeapTupleHeaderData* tup);

// HeapTupleSatisfiesAny — return true unconditionally (used by VACUUM).
bool HeapTupleSatisfiesAny(HeapTupleHeaderData* tup);

// HeapTupleIsSurelyDead — return true if the tuple is definitely dead
// (deleted by a committed transaction older than the snapshot's xmin).
// Used by VACUUM to decide which tuples can be reclaimed.
bool HeapTupleIsSurelyDead(const HeapTupleHeaderData* tup, const SnapshotData& snapshot);

// --- Visibility helpers (used internally, exposed for testing) ---

// XidVisibleInSnapshot — determine if a transaction ID is visible (committed)
// under the given snapshot. Returns true if committed and not in-progress.
// Sets *hint_committed if the XID is committed (for hint flag setting).
bool XidVisibleInSnapshot(TransactionId xid, const SnapshotData& snapshot, bool* hint_committed);

}  // namespace pgcpp::transaction

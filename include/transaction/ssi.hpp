// ssi.h — Serializable Snapshot Isolation (SSI) conflict detection.
//
// Converted from PostgreSQL 15's src/include/storage/predicate_internals.h
// and src/backend/storage/ipc/predicate.c.
//
// SSI provides true SERIALIZABLE isolation on top of Snapshot Isolation by
// detecting read/write conflicts that could produce non-serializable
// schedules. The core ideas:
//
//   1. Predicate locks (SIREAD locks) record what each serializable
//      transaction has read (tuple/page/relation granularity).
//   2. When a serializable transaction writes, it checks whether any other
//      serializable transaction holds a predicate lock covering the write
//      target. If so, a "rw-conflict" edge is recorded (reader → writer).
//   3. A "dangerous structure" is a 3-node antichain T1 → T2 → T3 where
//      T1 finished before T2's write and T2 finished before T3's write.
//      Such a structure cannot occur in any serial execution, so one of
//      the three transactions must be aborted with a serialization
//      failure.
//
// pgcpp is single-process, so true concurrency does not exist. However,
// the conflict detection machinery is preserved and is testable: tests
// can register multiple "concurrent" serializable transactions, simulate
// reads (via predicate locks), simulate writes, and verify that dangerous
// structures are detected and reported via ereport(ERROR).
//
// The existing pgcpp::storage predicate lock API (predicate.hpp) provides
// the SIREAD-lock storage layer; this module adds the per-transaction SSI
// state and the rw-conflict / dangerous-structure logic on top of it.
#pragma once

#include <cstdint>
#include <vector>

#include "storage/relfilenode.hpp"
#include "transaction/transam.hpp"

namespace pgcpp::transaction {

// RWConflict — a directed "read-write conflict" edge between two
// serializable transactions.
//
// Semantics: a conflict from `reader` to `writer` means the reader
// performed a read that the writer later conflicted with (either by
// writing the read tuple or by writing a tuple the reader's predicate
// lock covers). Matches PostgreSQL's RWConflict direction.
struct RWConflict {
    TransactionId reader = kInvalidTransactionId;
    TransactionId writer = kInvalidTransactionId;
};

// SERIALIZABLEXact — per-transaction SSI state.
//
// Each active or recently-finished serializable transaction has one of
// these. PostgreSQL stores this in shared memory keyed by XID; pgcpp
// stores it in an in-process vector (single-process limitation).
struct SERIALIZABLEXact {
    TransactionId xid = kInvalidTransactionId;
    bool finished = false;                  // committed or aborted
    bool committed = false;                 // true if finished by commit
    bool read_only = false;                 // READ ONLY transaction
    bool dangerous = false;                 // flagged as part of a dangerous structure
    std::vector<RWConflict> out_conflicts;  // edges where this tx is the reader
    std::vector<RWConflict> in_conflicts;   // edges where this tx is the writer
};

// RegisterSerializableTransaction — create the SSI state for `xid`.
// `read_only` marks the transaction as READ ONLY (skips write-side
// conflict checks). Idempotent: re-registering an existing xid is a no-op.
void RegisterSerializableTransaction(TransactionId xid, bool read_only);

// ReleaseSerializableTransaction — mark the SSI state for `xid` as
// finished (committed=true / aborted). Removes the xid's predicate locks
// and runs a final conflict check (a committing transaction may still be
// flagged dangerous by a concurrent writer — see OnConflict_CheckFor-
// SerializationFailure).
void ReleaseSerializableTransaction(TransactionId xid, bool committed);

// CheckForSerializableConflict — called when transaction `writer_xid`
// writes to (rnode, block, offset). Acquires no lock; instead it checks
// whether any *other* serializable transaction holds a predicate lock
// covering the target and, if so, records an rw-conflict edge and runs
// dangerous-structure detection. May ereport(ERROR) with a serialization
// failure.
void CheckForSerializableConflict(TransactionId writer_xid,
                                  const pgcpp::storage::RelFileNode& rnode, uint32_t block_num,
                                  uint16_t offset_num);

// CheckForDangerousStructure — detect the T1 → T2 → T3 antichain pattern
// centered on `xid` (which has just gained a new conflict edge). Returns
// true if a dangerous structure was found and at least one participant
// should be aborted.
bool CheckForDangerousStructure(TransactionId xid);

// OnConflict_CheckForSerializationFailure — if `xid` has been flagged as
// part of a dangerous structure, ereport(ERROR, ERRCODE_SERIALIZATION_-
// FAILURE) to abort it. Called at transaction commit and at write time.
void OnConflict_CheckForSerializationFailure(TransactionId xid);

// GetSerializableXact — return a pointer to the SSI state for `xid`,
// or nullptr if not registered. Used by tests.
SERIALIZABLEXact* GetSerializableXact(TransactionId xid);

// NumSerializableXacts — count of registered (including finished) SSI
// transactions. Used by tests.
int NumSerializableXacts();

// ResetSSIState — drop all SSI state (including the predicate lock store
// via PredicateLockReleaseAll). Used by tests.
void ResetSSIState();

}  // namespace pgcpp::transaction

// commit_ts.h — Commit timestamp tracking.
//
// Converted from PostgreSQL 15's src/include/access/commit_ts.h.
//
// Records the wall-clock timestamp at which each transaction committed.
// Used for logical replication and debugging. Backed by an SLRU in PG.
//
// pgcpp keeps an in-memory map (XID → timestamp). Timestamps are
// microseconds since the epoch (matches PG's TimestampTz internal rep).
#pragma once

#include <cstdint>
#include <vector>

#include "pgcpp/transaction/transam.hpp"

namespace pgcpp::transaction {

// TimestampTz — microseconds since 2000-01-01 00:00:00 UTC (matches PG).
using TimestampTz = int64_t;

// CommitTsEntry — per-transaction commit timestamp record (PG struct).
struct CommitTsEntry {
    TransactionId xid = kInvalidTransactionId;
    TimestampTz commit_ts = 0;  // 0 = not committed / unknown
};

// InitializeCommitTs — set up the commit timestamp subsystem (clear the table).
void InitializeCommitTs();

// ResetCommitTs — clear all commit timestamps (for testing).
void ResetCommitTs();

// TransactionIdSetCommitTs — record the commit timestamp for `xid`.
void TransactionIdSetCommitTs(TransactionId xid, TimestampTz ts);

// TransactionIdGetCommitTs — look up the commit timestamp for `xid`.
// Returns 0 if the transaction has no recorded commit time.
TimestampTz TransactionIdGetCommitTs(TransactionId xid);

// TransactionTreeSetCommitTsData — set commit timestamp for a transaction
// tree (top-level + subtransactions). In pgcpp, only the top-level XID
// is recorded (subtransactions inherit the parent's timestamp).
void TransactionTreeSetCommitTsData(TransactionId xid, TimestampTz ts, int nsubxids,
                                    const TransactionId* subxids);

}  // namespace pgcpp::transaction

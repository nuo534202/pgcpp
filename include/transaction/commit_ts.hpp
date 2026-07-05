// commit_ts.h — Commit timestamp tracking.
//
// Converted from PostgreSQL 15's src/include/access/commit_ts.h.
//
// Records the wall-clock timestamp at which each transaction committed.
// Used for logical replication and debugging. Backed by an SLRU in PG.
//
// pgcpp uses an SLRU with optional disk persistence (pg_commit_ts/).
// Each entry is 8 bytes (TimestampTz = int64_t), so 1024 entries per
// 8 KB page. The SLRU cache is process-local; disk is the source of truth.
#pragma once

#include <cstdint>
#include <string>

#include "transaction/slru.hpp"
#include "transaction/transam.hpp"

namespace pgcpp::transaction {

// TimestampTz — microseconds since 2000-01-01 00:00:00 UTC (matches PG).
using TimestampTz = int64_t;

// CommitTsEntry — per-transaction commit timestamp record (PG struct).
struct CommitTsEntry {
    TransactionId xid = kInvalidTransactionId;
    TimestampTz commit_ts = 0;  // 0 = not committed / unknown
};

// CommitTsEntriesPerPage — 8 KB / 8 bytes = 1024 entries per page.
constexpr int kCommitTsEntriesPerPage = kSlruPageSize / static_cast<int>(sizeof(TimestampTz));

// InitializeCommitTs — set up the commit timestamp subsystem.
// Call with an empty dir for in-memory operation (tests), or with
// <data_dir>/pg_commit_ts for persistence.
void InitializeCommitTs(const std::string& disk_dir = "");

// ResetCommitTs — clear all commit timestamps and the SLRU cache (for testing).
void ResetCommitTs();

// ShutdownCommitTs — flush dirty pages to disk.
void ShutdownCommitTs();

// FlushCommitTs — flush dirty pages to disk (called by checkpointer).
void FlushCommitTs();

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

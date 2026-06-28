// commit_ts.cpp — Commit timestamp tracking.
//
// Converted from PostgreSQL 15's src/backend/access/transam/commit_ts.cpp.
//
// Records the wall-clock timestamp at which each transaction committed.
// In PostgreSQL this is backed by an SLRU persisted to pg_commit_ts/.
// MyToyDB keeps an in-memory vector indexed by XID, which is sufficient for
// single-process operation.
#include "mytoydb/transaction/commit_ts.hpp"

#include <vector>

namespace mytoydb::transaction {

namespace {

// In-memory commit timestamp table: index by XID, value is the timestamp.
// 0 means "not committed / unknown". Uses a function-local static to avoid
// the global initialization order fiasco (Google C++ Style).
std::vector<TimestampTz>& CommitTsTable() {
    static std::vector<TimestampTz> table;
    return table;
}

}  // namespace

void InitializeCommitTs() {
    CommitTsTable().clear();
}

void ResetCommitTs() {
    CommitTsTable().clear();
}

void TransactionIdSetCommitTs(TransactionId xid, TimestampTz ts) {
    auto& table = CommitTsTable();
    if (xid >= table.size()) {
        table.resize(xid + 1, 0);
    }
    table[xid] = ts;
}

TimestampTz TransactionIdGetCommitTs(TransactionId xid) {
    auto& table = CommitTsTable();
    if (!TransactionIdIsValid(xid) || xid >= table.size()) {
        return 0;
    }
    return table[xid];
}

void TransactionTreeSetCommitTsData(TransactionId xid, TimestampTz ts, int nsubxids,
                                    const TransactionId* subxids) {
    TransactionIdSetCommitTs(xid, ts);
    for (int i = 0; i < nsubxids; ++i) {
        TransactionIdSetCommitTs(subxids[i], ts);
    }
}

}  // namespace mytoydb::transaction

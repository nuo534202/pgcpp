// commit_ts.cpp — Commit timestamp tracking.
//
// Converted from PostgreSQL 15's src/backend/access/transam/commit_ts.cpp.
//
// Records the wall-clock timestamp at which each transaction committed.
// In PostgreSQL this is backed by an SLRU persisted to pg_commit_ts/.
// pgcpp uses the SLRU infrastructure with optional disk persistence.
//
// Each entry is 8 bytes (TimestampTz = int64_t). Page size is 8 KB, so
// 1024 entries per page. XID → (pageno, offset) mapping:
//   pageno = xid / kCommitTsEntriesPerPage
//   offset = (xid % kCommitTsEntriesPerPage) * sizeof(TimestampTz)
#include "transaction/commit_ts.hpp"

#include <cstring>

namespace pgcpp::transaction {

namespace {

// The SLRU control block for commit timestamps. Nullptr means not initialized.
SlruCtl*& Ctl() {
    static SlruCtl* ctl = nullptr;
    return ctl;
}

}  // namespace

void InitializeCommitTs(const std::string& disk_dir) {
    if (Ctl() != nullptr) {
        SimpleLruFree(Ctl());
    }
    Ctl() = SimpleLruInit("commit_ts", /*capacity=*/16, disk_dir);
}

void ResetCommitTs() {
    if (Ctl() != nullptr) {
        SimpleLruReset(Ctl());
    }
}

void ShutdownCommitTs() {
    if (Ctl() != nullptr) {
        SimpleLruFlush(Ctl());
    }
}

void FlushCommitTs() {
    if (Ctl() != nullptr) {
        SimpleLruFlush(Ctl());
    }
}

void TransactionIdSetCommitTs(TransactionId xid, TimestampTz ts) {
    if (Ctl() == nullptr) {
        return;  // not initialized (test mode without InitializeCommitTs)
    }
    int pageno = static_cast<int>(xid / kCommitTsEntriesPerPage);
    int offset = static_cast<int>(xid % kCommitTsEntriesPerPage) *
                 static_cast<int>(sizeof(TimestampTz));
    SimpleLruWrite(Ctl(), pageno, offset, &ts, sizeof(ts));
}

TimestampTz TransactionIdGetCommitTs(TransactionId xid) {
    if (Ctl() == nullptr) {
        return 0;  // not initialized
    }
    if (!TransactionIdIsValid(xid)) {
        return 0;
    }
    int pageno = static_cast<int>(xid / kCommitTsEntriesPerPage);
    int offset = static_cast<int>(xid % kCommitTsEntriesPerPage) *
                 static_cast<int>(sizeof(TimestampTz));
    TimestampTz ts = 0;
    SimpleLruRead(Ctl(), pageno, offset, &ts, sizeof(ts));
    return ts;
}

void TransactionTreeSetCommitTsData(TransactionId xid, TimestampTz ts, int nsubxids,
                                    const TransactionId* subxids) {
    TransactionIdSetCommitTs(xid, ts);
    for (int i = 0; i < nsubxids; ++i) {
        TransactionIdSetCommitTs(subxids[i], ts);
    }
}

}  // namespace pgcpp::transaction

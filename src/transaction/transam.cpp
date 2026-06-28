// transam.cpp — Transaction ID type and commit-log status tracking.
//
// Converted from PostgreSQL 15's src/backend/access/transam/transam.cpp and
// src/backend/access/transam/clog.cpp (simplified for single-process).
//
// The commit log (CLOG) records the status (in-progress / committed /
// aborted) of every normal transaction ID. In PostgreSQL this is a set of
// shared-memory pages persisted to pg_xact/. pgcpp keeps an in-memory
// vector indexed by XID, which is sufficient for single-process operation.
#include "pgcpp/transaction/transam.hpp"

#include <vector>

#include "pgcpp/common/error/elog.hpp"

namespace pgcpp::transaction {

namespace {

// In-memory commit log: index by XID, value is the status.
// Index 0..2 (special XIDs) are never looked up (handled by the special-XID
// fast path in TransactionIdGetStatus).
std::vector<XidStatus>& CommitLog() {
    static std::vector<XidStatus> log;
    return log;
}

// The next XID to assign. Starts at FirstNormalTransactionId.
TransactionId& NextXid() {
    static TransactionId next = kFirstNormalTransactionId;
    return next;
}

// Single-entry cache for TransactionLogFetch (PostgreSQL's
// TransactionLogFetch keeps a one-row cache to avoid repeated CLOG reads).
TransactionId cached_xid = kInvalidTransactionId;
XidStatus cached_status = XidStatus::kInProgress;

// FetchFromCommitLog — read the raw status from the commit log without
// special-XID handling or caching.
XidStatus FetchFromCommitLog(TransactionId xid) {
    auto& log = CommitLog();
    if (xid >= log.size()) {
        // XID has not been recorded yet — treat as in-progress.
        return XidStatus::kInProgress;
    }
    return log[xid];
}

}  // namespace

void InitializeCommitLog() {
    auto& log = CommitLog();
    log.clear();
    // Reserve space for the first few transactions.
    log.resize(kFirstNormalTransactionId, XidStatus::kCommitted);
    // Mark special XIDs as committed (defensive; they should never be looked
    // up directly, but if they are, treat them as committed).
    log[kInvalidTransactionId] = XidStatus::kCommitted;
    log[kBootstrapTransactionId] = XidStatus::kCommitted;
    log[kFrozenTransactionId] = XidStatus::kCommitted;
    NextXid() = kFirstNormalTransactionId;
    // Clear the TransactionLogFetch cache.
    cached_xid = kInvalidTransactionId;
}

XidStatus TransactionLogFetch(TransactionId xid) {
    // Special XIDs are always considered committed; do not pollute the cache.
    if (!TransactionIdIsNormal(xid)) {
        return XidStatus::kCommitted;
    }
    if (xid == cached_xid) {
        return cached_status;
    }
    XidStatus status = FetchFromCommitLog(xid);
    cached_xid = xid;
    cached_status = status;
    return status;
}

XidStatus TransactionIdGetStatus(TransactionId xid) {
    return TransactionLogFetch(xid);
}

bool TransactionIdDidCommit(TransactionId xid) {
    XidStatus status = TransactionIdGetStatus(xid);
    return status == XidStatus::kCommitted;
}

bool TransactionIdDidAbort(TransactionId xid) {
    if (!TransactionIdIsNormal(xid)) {
        return false;  // special XIDs never abort
    }
    XidStatus status = TransactionIdGetStatus(xid);
    return status == XidStatus::kAborted;
}

void TransactionIdCommit(TransactionId xid) {
    if (!TransactionIdIsNormal(xid)) {
        ereport(pgcpp::error::LogLevel::kError,
                "cannot commit special transaction id " + std::to_string(xid));
    }

    auto& log = CommitLog();
    if (xid >= log.size()) {
        log.resize(xid + 1, XidStatus::kInProgress);
    }
    log[xid] = XidStatus::kCommitted;
    // Invalidate the TransactionLogFetch cache if it held this XID.
    if (xid == cached_xid) {
        cached_xid = kInvalidTransactionId;
    }
}

void TransactionIdAbort(TransactionId xid) {
    if (!TransactionIdIsNormal(xid)) {
        ereport(pgcpp::error::LogLevel::kError,
                "cannot abort special transaction id " + std::to_string(xid));
    }

    auto& log = CommitLog();
    if (xid >= log.size()) {
        log.resize(xid + 1, XidStatus::kInProgress);
    }
    log[xid] = XidStatus::kAborted;
    // Invalidate the TransactionLogFetch cache if it held this XID.
    if (xid == cached_xid) {
        cached_xid = kInvalidTransactionId;
    }
}

void TransactionIdSetInProgress(TransactionId xid) {
    if (!TransactionIdIsNormal(xid)) {
        return;  // ignore special XIDs
    }

    auto& log = CommitLog();
    if (xid >= log.size()) {
        log.resize(xid + 1, XidStatus::kInProgress);
    }
    log[xid] = XidStatus::kInProgress;
    // Invalidate the TransactionLogFetch cache if it held this XID.
    if (xid == cached_xid) {
        cached_xid = kInvalidTransactionId;
    }
}

TransactionId AllocateNextTransactionId() {
    TransactionId xid = NextXid();
    NextXid() = xid + 1;

    // Record as in-progress in the commit log.
    TransactionIdSetInProgress(xid);
    return xid;
}

TransactionId GetNextTransactionId() {
    return NextXid() - 1;
}

void ResetTransactionState() {
    InitializeCommitLog();
}

}  // namespace pgcpp::transaction

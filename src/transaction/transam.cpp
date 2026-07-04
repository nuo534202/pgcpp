// transam.cpp — Transaction ID type and commit-log status tracking.
//
// Converted from PostgreSQL 15's src/backend/access/transam/transam.cpp and
// src/backend/access/transam/clog.cpp.
//
// The commit log (CLOG) records the status (in-progress / committed /
// aborted) of every normal transaction ID. The VariableCache holds the
// next-XID counter. Both are allocated in shared memory (via
// ShmemInitStruct) so fork'd backends share the same transaction state.
// kCLogControlLock serializes mutations; kXactGenLock protects VariableCache.
// In test mode (no ShmemInit called), ShmemInitStruct falls back to
// process-local allocation.
#include "transaction/transam.hpp"

#include <cstring>
#include <vector>

#include "common/error/elog.hpp"
#include "storage/ipc/lwlock.hpp"
#include "storage/ipc/shmem.hpp"

namespace pgcpp::transaction {

namespace {

using pgcpp::storage::LookupNamedLock;
using pgcpp::storage::LWLock;
using pgcpp::storage::LWLockAcquire;
using pgcpp::storage::LWLockId;
using pgcpp::storage::LWLockMode;
using pgcpp::storage::LWLockRelease;
using pgcpp::storage::ShmemInitStruct;

// Pointer to the shm-allocated CLOG (kMaxXids entries, indexed by XID).
XidStatus* g_clog_base = nullptr;

// Pointer to the shm-allocated VariableCache (nextXid, oldestXid).
VariableCacheData* g_variable_cache = nullptr;

// Single-entry cache for TransactionLogFetch (per-backend, process-local).
// PostgreSQL keeps a one-row cache to avoid repeated CLOG reads.
TransactionId cached_xid = kInvalidTransactionId;
XidStatus cached_status = XidStatus::kInProgress;

// FetchFromCommitLog — read the raw status from the commit log without
// special-XID handling or caching.
XidStatus FetchFromCommitLog(TransactionId xid) {
    if (g_clog_base == nullptr) {
        return XidStatus::kInProgress;
    }
    if (xid >= static_cast<TransactionId>(kMaxXids)) {
        return XidStatus::kInProgress;
    }
    return g_clog_base[xid];
}

}  // namespace

void InitializeCommitLog() {
    // Always re-validate the pointers via ShmemInitStruct. In test mode,
    // ResetShmem() may have freed the backing memory, leaving g_clog_base
    // dangling. ShmemInitStruct is idempotent (returns existing region with
    // found=true if the name still exists, or allocates fresh with found=false).
    bool found_clog = false;
    g_clog_base = static_cast<XidStatus*>(ShmemInitStruct(
        "CommitLog", sizeof(XidStatus) * static_cast<std::size_t>(kMaxXids), &found_clog));

    bool found_vc = false;
    g_variable_cache = static_cast<VariableCacheData*>(
        ShmemInitStruct("VariableCache", sizeof(VariableCacheData), &found_vc));

    // Only zero/init if this is a fresh allocation (not yet initialized).
    if (!found_clog && g_clog_base != nullptr) {
        std::memset(g_clog_base, 0, sizeof(XidStatus) * static_cast<std::size_t>(kMaxXids));
        if (kMaxXids > 0)
            g_clog_base[kInvalidTransactionId] = XidStatus::kCommitted;
        if (kMaxXids > 1)
            g_clog_base[kBootstrapTransactionId] = XidStatus::kCommitted;
        if (kMaxXids > 2)
            g_clog_base[kFrozenTransactionId] = XidStatus::kCommitted;
    }

    if (!found_vc && g_variable_cache != nullptr) {
        g_variable_cache->nextXid = kFirstNormalTransactionId;
        g_variable_cache->oldestXid = kFirstNormalTransactionId;
    }

    // Clear the per-backend TransactionLogFetch cache.
    cached_xid = kInvalidTransactionId;
    cached_status = XidStatus::kInProgress;
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

    LWLock* lock = LookupNamedLock(LWLockId::kCLogControlLock);
    LWLockAcquire(lock, LWLockMode::kExclusive);

    if (g_clog_base != nullptr && xid < static_cast<TransactionId>(kMaxXids)) {
        g_clog_base[xid] = XidStatus::kCommitted;
    }

    LWLockRelease(lock);

    // Invalidate the per-backend TransactionLogFetch cache if it held this XID.
    if (xid == cached_xid) {
        cached_xid = kInvalidTransactionId;
    }
}

void TransactionIdAbort(TransactionId xid) {
    if (!TransactionIdIsNormal(xid)) {
        ereport(pgcpp::error::LogLevel::kError,
                "cannot abort special transaction id " + std::to_string(xid));
    }

    LWLock* lock = LookupNamedLock(LWLockId::kCLogControlLock);
    LWLockAcquire(lock, LWLockMode::kExclusive);

    if (g_clog_base != nullptr && xid < static_cast<TransactionId>(kMaxXids)) {
        g_clog_base[xid] = XidStatus::kAborted;
    }

    LWLockRelease(lock);

    // Invalidate the per-backend TransactionLogFetch cache if it held this XID.
    if (xid == cached_xid) {
        cached_xid = kInvalidTransactionId;
    }
}

void TransactionIdSetInProgress(TransactionId xid) {
    if (!TransactionIdIsNormal(xid)) {
        return;  // ignore special XIDs
    }

    LWLock* lock = LookupNamedLock(LWLockId::kCLogControlLock);
    LWLockAcquire(lock, LWLockMode::kExclusive);

    if (g_clog_base != nullptr && xid < static_cast<TransactionId>(kMaxXids)) {
        g_clog_base[xid] = XidStatus::kInProgress;
    }

    LWLockRelease(lock);

    // Invalidate the per-backend TransactionLogFetch cache if it held this XID.
    if (xid == cached_xid) {
        cached_xid = kInvalidTransactionId;
    }
}

TransactionId AllocateNextTransactionId() {
    LWLock* lock = LookupNamedLock(LWLockId::kXactGenLock);
    LWLockAcquire(lock, LWLockMode::kExclusive);

    TransactionId xid = kInvalidTransactionId;
    if (g_variable_cache != nullptr) {
        xid = g_variable_cache->nextXid;
        g_variable_cache->nextXid = xid + 1;
    }

    LWLockRelease(lock);

    if (xid == kInvalidTransactionId) {
        ereport(pgcpp::error::LogLevel::kError, "variable cache not initialized");
    }

    // Record as in-progress in the commit log.
    TransactionIdSetInProgress(xid);
    return xid;
}

TransactionId GetNextTransactionId() {
    LWLock* lock = LookupNamedLock(LWLockId::kXactGenLock);
    LWLockAcquire(lock, LWLockMode::kShared);
    TransactionId next =
        (g_variable_cache != nullptr) ? g_variable_cache->nextXid : kFirstNormalTransactionId;
    LWLockRelease(lock);
    return next - 1;
}

void ResetTransactionState() {
    // Re-initialize the CLOG and VariableCache in-place (clears contents
    // and resets nextXid). Does NOT call ResetShmem() — that would
    // invalidate other subsystems' shm pointers. The test fixture is
    // responsible for calling ResetShmem() if a completely clean slate
    // is needed.
    InitializeCommitLog();
}

std::size_t CLogShmemSize() {
    return sizeof(XidStatus) * static_cast<std::size_t>(kMaxXids) + sizeof(VariableCacheData);
}

}  // namespace pgcpp::transaction

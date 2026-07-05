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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "common/error/elog.hpp"
#include "storage/ipc/lwlock.hpp"
#include "storage/ipc/shmem.hpp"
#include "transaction/slru.hpp"

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

// --- CLOG persistence (pg_xact/) ---
//
// pgcpp persists CLOG pages to <clog_dir>/<hex_pageno> in PostgreSQL's
// 2-bit-per-XID format. The in-memory array uses 1 byte per XID for
// simplicity; the encoding is applied only on disk I/O.

std::string& ClogDirectory() {
    static std::string dir;
    return dir;
}

// Dirty page tracking: page numbers modified since the last FlushClogFiles.
std::set<int>& DirtyClogPages() {
    static std::set<int> pages;
    return pages;
}

// MarkDirty — record that the page containing `xid` has been modified.
void MarkDirty(TransactionId xid) {
    DirtyClogPages().insert(static_cast<int>(xid / kClogXidsPerPage));
}

// PageFileName — hex file name for a CLOG page (PG uses lowercase hex).
std::string PageFileName(int pageno) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04X", pageno);
    return std::string(buf);
}

// WriteAllToFd — write exactly `len` bytes to `fd`, retrying on EINTR.
// Returns true on success, false on write error.
bool WriteAllToFd(int fd, const uint8_t* data, std::size_t len) {
    std::size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, data + written, len - written);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
        written += static_cast<std::size_t>(n);
    }
    return true;
}

}  // namespace

void SetClogDirectory(const std::string& dir) {
    ClogDirectory() = dir;
}

void LoadClogFiles() {
    if (ClogDirectory().empty() || g_clog_base == nullptr) {
        return;
    }

    DIR* dir = opendir(ClogDirectory().c_str());
    if (dir == nullptr) {
        return;  // directory doesn't exist yet (fresh initdb)
    }

    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        // Skip "." and ".."
        if (ent->d_name[0] == '.')
            continue;

        // Parse filename as hex page number.
        char* end = nullptr;
        long pageno = std::strtol(ent->d_name, &end, 16);
        if (end == ent->d_name || *end != '\0' || pageno < 0) {
            continue;  // not a valid page file
        }

        // Read the page (up to 8 KB).
        std::string path = ClogDirectory() + "/" + ent->d_name;
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0)
            continue;

        uint8_t page[kSlruPageSize];
        ssize_t n = read(fd, page, kSlruPageSize);
        close(fd);
        if (n <= 0)
            continue;

        // Expand 2-bit entries to 1-byte and store in g_clog_base.
        TransactionId xid_start = static_cast<TransactionId>(pageno) * kClogXidsPerPage;
        for (ssize_t i = 0; i < n; ++i) {
            uint8_t byte = page[i];
            for (int j = 0; j < 4; ++j) {
                TransactionId xid = xid_start + i * 4 + j;
                if (xid >= static_cast<TransactionId>(kMaxXids))
                    break;
                uint8_t status = (byte >> (j * 2)) & 0x03;
                g_clog_base[xid] = static_cast<XidStatus>(status);
            }
        }
    }
    closedir(dir);
}

void FlushClogFiles() {
    if (ClogDirectory().empty() || g_clog_base == nullptr) {
        return;
    }

    // Create directory if it doesn't exist.
    mkdir(ClogDirectory().c_str(), 0700);

    for (int pageno : DirtyClogPages()) {
        std::string path = ClogDirectory() + "/" + PageFileName(pageno);
        int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
        if (fd < 0)
            continue;

        // Encode 1-byte-per-XID → 2-bit-per-XID page.
        uint8_t page[kSlruPageSize];
        std::memset(page, 0, sizeof(page));
        TransactionId xid_start = static_cast<TransactionId>(pageno) * kClogXidsPerPage;
        for (int i = 0; i < kSlruPageSize; ++i) {
            for (int j = 0; j < 4; ++j) {
                TransactionId xid = xid_start + i * 4 + j;
                if (xid >= static_cast<TransactionId>(kMaxXids))
                    break;
                uint8_t status = static_cast<uint8_t>(g_clog_base[xid]);
                page[i] |= (status & 0x03) << (j * 2);
            }
        }

        WriteAllToFd(fd, page, kSlruPageSize);
        fsync(fd);
        close(fd);
    }
    DirtyClogPages().clear();
}

void ShutdownClog() {
    FlushClogFiles();
}

void ResetClogPersistence() {
    ClogDirectory().clear();
    DirtyClogPages().clear();
}

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
        MarkDirty(xid);
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
        MarkDirty(xid);
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
        MarkDirty(xid);
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

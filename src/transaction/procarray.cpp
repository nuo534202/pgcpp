// procarray.cpp — Process array: tracks all running transactions.
//
// Converted from PostgreSQL 15's src/backend/storage/ipc/procarray.cpp.
//
// ProcArray is the authoritative list of all currently-running backend
// transactions. It is used to compute snapshots (which XIDs are visible),
// determine the oldest running XID (for VACUUM), and build the snapshot of
// running XIDs for standby.
//
// pgcpp allocates the ProcArray as a shared-memory array of TransactionIds
// (via ShmemInitStruct) so fork'd backends share the same view. All
// mutations are serialized by kProcArrayLock. In test mode (no ShmemInit
// called), ShmemInitStruct falls back to process-local allocation.
#include "transaction/procarray.hpp"

#include <algorithm>
#include <vector>

#include "storage/ipc/lwlock.hpp"
#include "storage/ipc/proc.hpp"
#include "storage/ipc/shmem.hpp"
#include "transaction/transam.hpp"

namespace pgcpp::transaction {

namespace {

using pgcpp::storage::LWLock;
using pgcpp::storage::LWLockAcquire;
using pgcpp::storage::LWLockId;
using pgcpp::storage::LWLockMode;
using pgcpp::storage::LWLockRelease;
using pgcpp::storage::LookupNamedLock;
using pgcpp::storage::kMaxBackends;
using pgcpp::storage::ShmemInitStruct;

// Pointer to the shm-allocated ProcArray (kMaxBackends entries).
TransactionId* g_procarray_xids = nullptr;

// Number of active entries in g_procarray_xids. Protected by kProcArrayLock.
int g_procarray_count = 0;

}  // namespace

void InitializeProcArray() {
    // Always re-validate the pointer via ShmemInitStruct. In test mode,
    // ResetShmem() may have freed the backing memory, leaving g_procarray_xids
    // dangling. ShmemInitStruct is idempotent (returns existing region with
    // found=true if the name still exists, or allocates fresh with found=false).
    // This mirrors the InitializeCommitLog() defensive pattern.
    bool found = false;
    g_procarray_xids = static_cast<TransactionId*>(
        ShmemInitStruct("ProcArrayXids",
                        sizeof(TransactionId) * static_cast<std::size_t>(kMaxBackends),
                        &found));

    if (!found && g_procarray_xids != nullptr) {
        g_procarray_count = 0;
        for (int i = 0; i < kMaxBackends; ++i) {
            g_procarray_xids[i] = kInvalidTransactionId;
        }
    }
}

void ResetProcArray() {
    // Re-validate the pointer in case ResetShmem() freed the backing memory
    // since the last InitializeProcArray(). If shmem was reset, this
    // allocates a fresh region and zeros it; otherwise it returns the
    // existing region which we then clear.
    InitializeProcArray();
    if (g_procarray_xids != nullptr) {
        for (int i = 0; i < kMaxBackends; ++i) {
            g_procarray_xids[i] = kInvalidTransactionId;
        }
        g_procarray_count = 0;
    }
    // Note: we intentionally do NOT call ResetShmem() here — that would
    // invalidate other subsystems' shm pointers (CLOG, VariableCache, etc.).
    // Each subsystem manages its own state; the test fixture is responsible
    // for calling ResetShmem() if a completely clean slate is needed.
}

void ProcArrayAdd(TransactionId xid) {
    InitializeProcArray();

    LWLock* lock = LookupNamedLock(LWLockId::kProcArrayLock);
    LWLockAcquire(lock, LWLockMode::kExclusive);

    if (g_procarray_count < kMaxBackends) {
        g_procarray_xids[g_procarray_count++] = xid;
    }

    LWLockRelease(lock);
}

void ProcArrayRemove(TransactionId xid) {
    if (g_procarray_xids == nullptr) {
        return;
    }

    LWLock* lock = LookupNamedLock(LWLockId::kProcArrayLock);
    LWLockAcquire(lock, LWLockMode::kExclusive);

    // Find and remove the xid (swap with last element for O(1) removal).
    for (int i = 0; i < g_procarray_count; ++i) {
        if (g_procarray_xids[i] == xid) {
            // Move the last element into this slot.
            g_procarray_count--;
            g_procarray_xids[i] = g_procarray_xids[g_procarray_count];
            g_procarray_xids[g_procarray_count] = kInvalidTransactionId;
            break;
        }
    }

    LWLockRelease(lock);
}

TransactionId GetOldestXmin(TransactionId ignore) {
    if (g_procarray_xids == nullptr) {
        return kFrozenTransactionId;
    }

    LWLock* lock = LookupNamedLock(LWLockId::kProcArrayLock);
    LWLockAcquire(lock, LWLockMode::kShared);

    TransactionId oldest = kInvalidTransactionId;
    for (int i = 0; i < g_procarray_count; ++i) {
        TransactionId xid = g_procarray_xids[i];
        if (ignore != kInvalidTransactionId && xid == ignore) {
            continue;
        }
        if (oldest == kInvalidTransactionId || TransactionIdPrecedes(xid, oldest)) {
            oldest = xid;
        }
    }

    LWLockRelease(lock);

    if (oldest == kInvalidTransactionId) {
        return kFrozenTransactionId;
    }
    return oldest;
}

std::vector<TransactionId> GetRunningTransactionData() {
    std::vector<TransactionId> result;
    TransactionId xmax, xmin;
    GetRunningTransactionData(result, &xmax, &xmin);
    return result;
}

void GetRunningTransactionData(std::vector<TransactionId>& xip_out,
                               TransactionId* xmax_out,
                               TransactionId* xmin_out) {
    xip_out.clear();

    if (g_procarray_xids == nullptr) {
        if (xmax_out != nullptr) *xmax_out = GetNextTransactionId() + 1;
        if (xmin_out != nullptr) *xmin_out = kFrozenTransactionId;
        return;
    }

    LWLock* lock = LookupNamedLock(LWLockId::kProcArrayLock);
    LWLockAcquire(lock, LWLockMode::kShared);

    for (int i = 0; i < g_procarray_count; ++i) {
        xip_out.push_back(g_procarray_xids[i]);
    }

    LWLockRelease(lock);

    if (xmax_out != nullptr) {
        *xmax_out = GetNextTransactionId() + 1;
    }
    if (xmin_out != nullptr) {
        TransactionId oldest = kInvalidTransactionId;
        for (TransactionId xid : xip_out) {
            if (oldest == kInvalidTransactionId || TransactionIdPrecedes(xid, oldest)) {
                oldest = xid;
            }
        }
        *xmin_out = (oldest == kInvalidTransactionId) ? kFrozenTransactionId : oldest;
    }
}

int CountRunningXacts() {
    if (g_procarray_xids == nullptr) {
        return 0;
    }

    LWLock* lock = LookupNamedLock(LWLockId::kProcArrayLock);
    LWLockAcquire(lock, LWLockMode::kShared);
    int count = g_procarray_count;
    LWLockRelease(lock);
    return count;
}

bool ProcArrayContains(TransactionId xid) {
    if (g_procarray_xids == nullptr) {
        return false;
    }

    LWLock* lock = LookupNamedLock(LWLockId::kProcArrayLock);
    LWLockAcquire(lock, LWLockMode::kShared);

    bool found = false;
    for (int i = 0; i < g_procarray_count; ++i) {
        if (g_procarray_xids[i] == xid) {
            found = true;
            break;
        }
    }

    LWLockRelease(lock);
    return found;
}

std::size_t ProcArrayShmemSize() {
    return sizeof(TransactionId) * static_cast<std::size_t>(kMaxBackends);
}

}  // namespace pgcpp::transaction

// procarray.cpp — Process array: tracks all running transactions.
//
// Converted from PostgreSQL 15's src/backend/storage/ipc/procarray.cpp.
//
// ProcArray is the authoritative list of all currently-running backend
// transactions. It is used to compute snapshots (which XIDs are visible),
// determine the oldest running XID (for VACUUM), and build the snapshot of
// running XIDs for standby.
//
// pgcpp allocates the ProcArray in shared memory as an array of PGPROC pool
// indices (pgprocnos), parallel to the PGXACT compact array. InitProcess
// registers a backend by calling ProcArrayAdd(PGPROC*); ProcKill deregisters
// via ProcArrayRemove(PGPROC*). GetSnapshotData scans the PGXACT entries
// (via pgprocnos) for cache-line efficiency. All mutations are serialized
// by kProcArrayLock. In test mode (no ShmemInit called), ShmemInitStruct
// falls back to process-local allocation.
#include "transaction/procarray.hpp"

#include <algorithm>
#include <vector>

#include "storage/ipc/lwlock.hpp"
#include "storage/ipc/proc.hpp"
#include "storage/ipc/shmem.hpp"
#include "transaction/transam.hpp"

namespace pgcpp::transaction {

namespace {

using pgcpp::storage::GetPgXactByIndex;
using pgcpp::storage::kMaxBackends;
using pgcpp::storage::LookupNamedLock;
using pgcpp::storage::LWLock;
using pgcpp::storage::LWLockAcquire;
using pgcpp::storage::LWLockId;
using pgcpp::storage::LWLockMode;
using pgcpp::storage::LWLockRelease;
using pgcpp::storage::PGPROC;
using pgcpp::storage::PGXACT;
using pgcpp::storage::ShmemInitStruct;

// ProcArrayStruct — the shared-memory header for the ProcArray.
// Stores pgprocnos (indices into the PGPROC/PGXACT pool) of all registered
// backends. numProcs is the number of valid entries.
struct ProcArrayStruct {
    int pgprocnos[kMaxBackends];  // PGPROC pool indices of registered backends
    int numProcs = 0;             // number of valid entries in pgprocnos
};

// Pointer to the shm-allocated ProcArray header.
ProcArrayStruct* g_procarray = nullptr;

}  // namespace

void InitializeProcArray() {
    // Always re-validate the pointer via ShmemInitStruct. In test mode,
    // ResetShmem() may have freed the backing memory, leaving g_procarray
    // dangling. ShmemInitStruct is idempotent (returns existing region with
    // found=true if the name still exists, or allocates fresh with found=false).
    bool found = false;
    g_procarray = static_cast<ProcArrayStruct*>(
        ShmemInitStruct("ProcArray", sizeof(ProcArrayStruct), &found));

    if (!found && g_procarray != nullptr) {
        g_procarray->numProcs = 0;
        for (int i = 0; i < kMaxBackends; ++i) {
            g_procarray->pgprocnos[i] = -1;
        }
    }
}

void ResetProcArray() {
    // Re-validate the pointer in case ResetShmem() freed the backing memory
    // since the last InitializeProcArray().
    InitializeProcArray();
    if (g_procarray != nullptr) {
        for (int i = 0; i < kMaxBackends; ++i) {
            g_procarray->pgprocnos[i] = -1;
        }
        g_procarray->numProcs = 0;
    }
    // Note: we intentionally do NOT call ResetShmem() here — that would
    // invalidate other subsystems' shm pointers (CLOG, VariableCache, etc.).
}

void ProcArrayAdd(PGPROC* proc) {
    InitializeProcArray();

    if (proc == nullptr || proc->pgprocno < 0) {
        return;
    }

    LWLock* lock = LookupNamedLock(LWLockId::kProcArrayLock);
    LWLockAcquire(lock, LWLockMode::kExclusive);

    if (g_procarray != nullptr && g_procarray->numProcs < kMaxBackends) {
        g_procarray->pgprocnos[g_procarray->numProcs++] = proc->pgprocno;
    }

    LWLockRelease(lock);
}

void ProcArrayRemove(PGPROC* proc) {
    if (g_procarray == nullptr || proc == nullptr || proc->pgprocno < 0) {
        return;
    }

    LWLock* lock = LookupNamedLock(LWLockId::kProcArrayLock);
    LWLockAcquire(lock, LWLockMode::kExclusive);

    // Find and remove the pgprocno (swap with last element for O(1) removal).
    for (int i = 0; i < g_procarray->numProcs; ++i) {
        if (g_procarray->pgprocnos[i] == proc->pgprocno) {
            g_procarray->numProcs--;
            g_procarray->pgprocnos[i] = g_procarray->pgprocnos[g_procarray->numProcs];
            g_procarray->pgprocnos[g_procarray->numProcs] = -1;
            break;
        }
    }

    LWLockRelease(lock);
}

TransactionId GetOldestXmin(TransactionId ignore) {
    if (g_procarray == nullptr) {
        return kFrozenTransactionId;
    }

    LWLock* lock = LookupNamedLock(LWLockId::kProcArrayLock);
    LWLockAcquire(lock, LWLockMode::kShared);

    TransactionId oldest = kInvalidTransactionId;
    for (int i = 0; i < g_procarray->numProcs; ++i) {
        int pgprocno = g_procarray->pgprocnos[i];
        PGXACT* pgxact = GetPgXactByIndex(pgprocno);
        if (pgxact == nullptr) {
            continue;
        }
        TransactionId xid = pgxact->xid;
        if (!TransactionIdIsValid(xid)) {
            continue;
        }
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

void GetRunningTransactionData(std::vector<TransactionId>& xip_out, TransactionId* xmax_out,
                               TransactionId* xmin_out) {
    xip_out.clear();

    if (g_procarray == nullptr) {
        if (xmax_out != nullptr)
            *xmax_out = GetNextTransactionId() + 1;
        if (xmin_out != nullptr)
            *xmin_out = kFrozenTransactionId;
        return;
    }

    LWLock* lock = LookupNamedLock(LWLockId::kProcArrayLock);
    LWLockAcquire(lock, LWLockMode::kShared);

    // Scan the PGXACT compact array (via pgprocnos) for cache-line efficiency.
    for (int i = 0; i < g_procarray->numProcs; ++i) {
        int pgprocno = g_procarray->pgprocnos[i];
        PGXACT* pgxact = GetPgXactByIndex(pgprocno);
        if (pgxact == nullptr) {
            continue;
        }
        TransactionId xid = pgxact->xid;
        if (TransactionIdIsValid(xid)) {
            xip_out.push_back(xid);
        }
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
    if (g_procarray == nullptr) {
        return 0;
    }

    LWLock* lock = LookupNamedLock(LWLockId::kProcArrayLock);
    LWLockAcquire(lock, LWLockMode::kShared);
    int count = g_procarray->numProcs;
    LWLockRelease(lock);
    return count;
}

bool ProcArrayContains(TransactionId xid) {
    if (g_procarray == nullptr) {
        return false;
    }

    LWLock* lock = LookupNamedLock(LWLockId::kProcArrayLock);
    LWLockAcquire(lock, LWLockMode::kShared);

    bool found = false;
    for (int i = 0; i < g_procarray->numProcs; ++i) {
        int pgprocno = g_procarray->pgprocnos[i];
        PGXACT* pgxact = GetPgXactByIndex(pgprocno);
        if (pgxact != nullptr && pgxact->xid == xid) {
            found = true;
            break;
        }
    }

    LWLockRelease(lock);
    return found;
}

std::size_t ProcArrayShmemSize() {
    return sizeof(ProcArrayStruct);
}

}  // namespace pgcpp::transaction

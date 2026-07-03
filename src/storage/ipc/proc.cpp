// proc.cpp — PGPROC struct (per-backend state) and process array helpers.
//
// Converted from PostgreSQL 15's src/backend/storage/ipc/proc.c.
//
// The PGPROC pool is allocated in shared memory (via ShmemInitStruct) so
// fork'd backends share the same pool. InitProcess claims a slot from the
// freelist under kProcArrayLock; ProcKill returns it. In test mode (no
// ShmemInit called), ShmemInitStruct falls back to process-local allocation.
#include "storage/ipc/proc.hpp"

#include <unistd.h>
#include <vector>

#include "storage/ipc/lwlock.hpp"
#include "storage/ipc/shmem.hpp"

namespace pgcpp::storage {

namespace {

// Pointer to the shm-allocated PGPROC pool (kMaxBackends entries).
// Set by ProcGlobalInit(); nullptr before initialization.
PGPROC* g_proc_base = nullptr;

// Head of the PGPROC freelist (slots with pid == 0). Protected by
// kProcArrayLock. Linked via PGPROC::next.
PGPROC* g_proc_freelist = nullptr;

// Per-backend pointer to the current PGPROC. Each fork'd child has its own
// copy of this static (correctly per-process).
PGPROC*& MyProcPtr() {
    static PGPROC* p = nullptr;
    return p;
}

// Per-backend local XID counter (resets with ProcGlobalReset).
int& NextLxid() {
    static int x = 1;
    return x;
}

}  // namespace

PGPROC* InitProcess() {
    ProcGlobalInit();

    LWLock* lock = LookupNamedLock(LWLockId::kProcArrayLock);
    LWLockAcquire(lock, LWLockMode::kExclusive);

    if (g_proc_freelist == nullptr) {
        LWLockRelease(lock);
        return nullptr;  // no slots available
    }

    PGPROC* p = g_proc_freelist;
    g_proc_freelist = g_proc_freelist->next;
    p->next = nullptr;

    p->pid = static_cast<int>(::getpid());
    p->lxid = NextLxid()++;
    p->xid = pgcpp::transaction::kInvalidTransactionId;
    p->xmin = pgcpp::transaction::kInvalidTransactionId;
    p->status = ProcStatus::kIdle;
    p->procLatch = nullptr;
    p->backend_id[0] = '\0';
    p->backend_id_num = 0;
    p->is_aux = false;

    MyProcPtr() = p;

    LWLockRelease(lock);
    return p;
}

void ProcKill() {
    PGPROC* p = MyProcPtr();
    if (p == nullptr) {
        return;
    }

    LWLock* lock = LookupNamedLock(LWLockId::kProcArrayLock);
    LWLockAcquire(lock, LWLockMode::kExclusive);

    // Return the slot to the freelist.
    p->pid = 0;
    p->xid = pgcpp::transaction::kInvalidTransactionId;
    p->xmin = pgcpp::transaction::kInvalidTransactionId;
    p->next = g_proc_freelist;
    g_proc_freelist = p;

    MyProcPtr() = nullptr;

    LWLockRelease(lock);
}

void ProcGlobalInit() {
    if (g_proc_base != nullptr) {
        return;  // already initialized
    }

    bool found = false;
    g_proc_base = static_cast<PGPROC*>(
        ShmemInitStruct("ProcPool", sizeof(PGPROC) * kMaxBackends, &found));

    if (!found) {
        // Build the freelist: link all slots in order.
        for (int i = 0; i < kMaxBackends; ++i) {
            g_proc_base[i] = PGPROC{};  // zero-init
            g_proc_base[i].next = (i + 1 < kMaxBackends) ? &g_proc_base[i + 1] : nullptr;
        }
        g_proc_freelist = &g_proc_base[0];
    } else {
        // Re-attach: rebuild the freelist from slots with pid == 0.
        // (In multi-process mode, fork'd children inherit the freelist
        // pointer via MAP_SHARED, so this branch is mainly for test mode
        // where ShmemInitStruct returns the same process-local memory.)
        g_proc_freelist = nullptr;
        for (int i = kMaxBackends - 1; i >= 0; --i) {
            if (g_proc_base[i].pid == 0) {
                g_proc_base[i].next = g_proc_freelist;
                g_proc_freelist = &g_proc_base[i];
            }
        }
    }
}

void ProcGlobalReset() {
    // In test mode, ResetShmem() may have already freed the backing memory,
    // so we can't safely dereference g_proc_base. Just drop the pointers;
    // the next ProcGlobalInit() / InitProcess() will re-allocate via
    // ShmemInitStruct (which gives fresh memory in test mode).
    if (!IsShmemActive()) {
        g_proc_base = nullptr;
        g_proc_freelist = nullptr;
    } else {
        // Multi-process mode: clear slots in-place (shm survives).
        if (g_proc_base != nullptr) {
            for (int i = 0; i < kMaxBackends; ++i) {
                g_proc_base[i] = PGPROC{};
            }
            g_proc_freelist = nullptr;
            for (int i = 0; i < kMaxBackends; ++i) {
                g_proc_base[i].next = (i + 1 < kMaxBackends) ? &g_proc_base[i + 1] : nullptr;
            }
            g_proc_freelist = &g_proc_base[0];
        }
    }
    MyProcPtr() = nullptr;
    NextLxid() = 1;
}

PGPROC* GetMyProc() {
    return MyProcPtr();
}

void SetMyProc(PGPROC* proc) {
    MyProcPtr() = proc;
}

std::size_t ProcArrayShmemSize() {
    return sizeof(PGPROC) * static_cast<std::size_t>(kMaxBackends);
}

int NumProcs() {
    if (g_proc_base == nullptr) {
        return 0;
    }
    int count = 0;
    for (int i = 0; i < kMaxBackends; ++i) {
        if (g_proc_base[i].pid != 0) {
            ++count;
        }
    }
    return count;
}

std::vector<PGPROC*> AllProcs() {
    std::vector<PGPROC*> result;
    if (g_proc_base == nullptr) {
        return result;
    }
    for (int i = 0; i < kMaxBackends; ++i) {
        if (g_proc_base[i].pid != 0) {
            result.push_back(&g_proc_base[i]);
        }
    }
    return result;
}

}  // namespace pgcpp::storage

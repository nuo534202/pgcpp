// proc.cpp — PGPROC struct (per-backend state) and process array helpers.
//
// Converted from PostgreSQL 15's src/backend/storage/ipc/proc.c.
#include "pgcpp/storage/ipc/proc.hpp"

#include <unistd.h>

#include <deque>
#include <vector>

namespace mytoydb::storage {

namespace {

// ProcPool — the array of currently-active PGPROCs.
// We use std::deque (not std::vector) because deque does not invalidate
// pointers to its elements when push_back is called — important since
// callers hold PGPROC* pointers across calls.
std::deque<PGPROC>& ProcPool() {
    static std::deque<PGPROC> pool;
    return pool;
}

// MyProcPtr — pointer to the current backend's PGPROC (set by InitProcess).
PGPROC*& MyProcPtr() {
    static PGPROC* p = nullptr;
    return p;
}

// NextLxid — local XID counter (resets with ProcGlobalReset).
int& NextLxid() {
    static int x = 1;
    return x;
}

}  // namespace

PGPROC* InitProcess() {
    ProcGlobalInit();
    auto& pool = ProcPool();
    PGPROC proc;
    proc.pid = static_cast<int>(::getpid());
    proc.lxid = NextLxid()++;
    pool.push_back(std::move(proc));
    PGPROC* p = &pool.back();
    MyProcPtr() = p;
    return p;
}

void ProcGlobalInit() {
    // Idempotent: nothing to do unless the pool is uninitialized.
    (void)ProcPool();
}

void ProcGlobalReset() {
    ProcPool().clear();
    MyProcPtr() = nullptr;
    NextLxid() = 1;
}

PGPROC* GetMyProc() {
    return MyProcPtr();
}

void SetMyProc(PGPROC* proc) {
    MyProcPtr() = proc;
}

int ProcArrayShmemSize() {
    return static_cast<int>(ProcPool().size()) * static_cast<int>(sizeof(PGPROC));
}

int NumProcs() {
    return static_cast<int>(ProcPool().size());
}

std::vector<PGPROC*> AllProcs() {
    std::vector<PGPROC*> result;
    for (auto& proc : ProcPool()) {
        result.push_back(&proc);
    }
    return result;
}

}  // namespace mytoydb::storage

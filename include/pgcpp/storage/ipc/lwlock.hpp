// lwlock.h — Lightweight reader/writer locks.
//
// Converted from PostgreSQL 15's src/include/storage/lwlock.h and
// src/backend/storage/ipc/lwlock.c.
//
// LWLocks are the PG native read/write lock primitive used by buffer
// descriptors, the proc array, the WAL, and many other subsystems. The
// "LW" stands for "lightweight" — they are faster than the heavyweight
// regular lock manager (lmgr) because they don't have a deadlock detector
// and use a small fixed-size lock array.
//
// pgcpp is single-process, so LWLocks never actually block: Acquire
// succeeds immediately and just tracks the held count for assertions.
// The API is preserved for architectural fidelity.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pgcpp::storage {

// LWLockMode — lock mode (mirrors PG's LW_EXCLUSIVE / LW_SHARED).
enum class LWLockMode {
    kExclusive,  // LW_EXCLUSIVE — single writer, no other holders
    kShared,     // LW_SHARED — multiple readers, no writers
};

// LWLockId — builtin named lock identifiers (subset of PG's Builtin LWLocks).
enum class LWLockId : int {
    kShmemIndexLock = 0,
    kOidGenLock,
    kXactGenLock,
    kProcArrayLock,
    kSInvalReadLock,
    kSInvalWriteLock,
    kWALBufMappingLock,
    kWALWriteLock,
    kControlFileLock,
    kCheckpointLock,
    kCLogControlLock,
    kSubtransControlLock,
    kMultiXactGenLock,
    kMultiXactTruncationLock,
    kAutovacuumLock,
    kAutovacuumScheduleLock,
    kSyncScanListsLock,
    kDynamicSharedMemoryControlLock,
    kLockManagerLock,
    kPredicateLockManagerLock,
    kOldSerXidLock,
    kWrapLimitsVacuumLock,
    kBufferIOCount,
};

// LWLock — a single read/write lock instance.
struct LWLock {
    int shared_holders = 0;       // count of SHARED holders
    bool exclusive_held = false;  // true if EXCLUSIVE held
    LWLockId id;                  // lock id (for debugging)
    int tranche = 0;              // tranche id (PG's per-tranche arrays)
};

// LWLockInitialize — create a new LWLock with the given id/tranche.
void LWLockInitialize(LWLock* lock, LWLockId id, int tranche = 0);

// LWLockAcquire — acquire a lock in the given mode.
// In pgcpp (single-process) this never blocks; it asserts invariants and
// updates bookkeeping. Returns true on success.
bool LWLockAcquire(LWLock* lock, LWLockMode mode);

// LWLockRelease — release a previously-acquired lock.
void LWLockRelease(LWLock* lock);

// LWLockHeldByMe — true if the current backend holds this lock in any mode.
bool LWLockHeldByMe(const LWLock* lock);

// LWLockHeldByMeInMode — true if the current backend holds this lock in the
// specified mode.
bool LWLockHeldByMeInMode(const LWLock* lock, LWLockMode mode);

// LWLockConditionalAcquire — try to acquire without blocking (always succeeds
// in single-process pgcpp if no other holder exists).
bool LWLockConditionalAcquire(LWLock* lock, LWLockMode mode);

// --- Named lock table (builtin tranche) ---

// LookupNamedLock — return the LWLock for a builtin id, creating it if needed.
LWLock* LookupNamedLock(LWLockId id);

// InitializeAllLWLocks — allocate and initialize the builtin lock array.
// Idempotent.
void InitializeAllLWLocks();

// ResetAllLWLocks — drop all LWLock state (used by tests).
void ResetAllLWLocks();

// NumHeldLWLocks — total number of LWLocks currently held by this backend.
int NumHeldLWLocks();

// HeldLWLockIds — return the ids of all currently-held locks.
std::vector<LWLockId> HeldLWLockIds();

}  // namespace pgcpp::storage

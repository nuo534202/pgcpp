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
// pgcpp uses std::atomic for the lock state so it works correctly across
// fork'd processes sharing memory via MAP_SHARED. The lock array is
// allocated in shared memory (via ShmemInitStruct). A spin-then-yield
// strategy is used for contention; in single-process/test mode there is
// never contention so the spin loop exits immediately.
#pragma once

#include <atomic>
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
    kBufferMappingLock,  // legacy single lock (unused for partitioned access)
    kBufFreelistLock,    // protects clock sweep + free list
    kBufferIOCount,      // sentinel = first unused id
};

// kNumBufferPartitions — number of BufMappingLock partitions (PG default: 16).
// Each partition protects a slice of the buffer hash table, reducing contention
// versus a single global lock. Declared here so the LWLock allocator can size
// the partition lock array; BufMappingPartition() in buf_internals.hpp maps a
// BufferTag to a partition index.
constexpr int kNumBufferPartitions = 16;

// Total number of builtin named LWLocks.
constexpr int kNumNamedLWLocks = static_cast<int>(LWLockId::kBufferIOCount) + 1;

// LWLock — a single read/write lock instance.
//
// The state is packed into a single atomic word:
//   bit 31: exclusive held
//   bits 0-30: shared holder count
// This allows atomic CAS operations for both acquire modes.
struct LWLock {
    std::atomic<uint32_t> state{0};
    LWLockId id = LWLockId::kShmemIndexLock;
    int tranche = 0;

    // Accessors for the packed state (for tests/inspection).
    int SharedHolders() const { return static_cast<int>(state.load() & 0x7FFFFFFF); }
    bool ExclusiveHeld() const { return (state.load() & 0x80000000u) != 0; }
};

// GetBufMappingLock — return the LWLock for the given buffer-mapping partition
// (0..kNumBufferPartitions-1). The partition lock array is allocated in shared
// memory alongside the named lock array. Used by the buffer manager for
// hash-table lookups/insertions.
LWLock* GetBufMappingLock(int partition);

// LWLockInitialize — create a new LWLock with the given id/tranche.
void LWLockInitialize(LWLock* lock, LWLockId id, int tranche = 0);

// LWLockAcquire — acquire a lock in the given mode.
// Spins (with sched_yield) until the lock is available. Returns true on
// success (always succeeds eventually unless a deadlock occurs).
bool LWLockAcquire(LWLock* lock, LWLockMode mode);

// LWLockRelease — release a previously-acquired lock.
void LWLockRelease(LWLock* lock);

// LWLockHeldByMe — true if the current backend holds this lock in any mode.
bool LWLockHeldByMe(const LWLock* lock);

// LWLockHeldByMeInMode — true if the current backend holds this lock in the
// specified mode.
bool LWLockHeldByMeInMode(const LWLock* lock, LWLockMode mode);

// LWLockConditionalAcquire — try to acquire without blocking. Returns true
// on success, false if the lock is held by another mode.
bool LWLockConditionalAcquire(LWLock* lock, LWLockMode mode);

// --- Named lock table (builtin tranche) ---

// LookupNamedLock — return the LWLock for a builtin id. The lock array is
// allocated in shared memory (via ShmemInitStruct) so it is shared across
// fork'd processes.
LWLock* LookupNamedLock(LWLockId id);

// InitializeAllLWLocks — allocate and initialize the builtin lock array
// in shared memory. Idempotent.
void InitializeAllLWLocks();

// ResetAllLWLocks — drop all LWLock state (used by tests).
void ResetAllLWLocks();

// ResetHeldLWLocks — clear this backend's per-process held-lock tracking.
// Called by a fork'd child after fork to avoid inheriting the parent's
// held-lock list.
void ResetHeldLWLocks();

// NumHeldLWLocks — total number of LWLocks currently held by this backend.
int NumHeldLWLocks();

// HeldLWLockIds — return the ids of all currently-held locks.
std::vector<LWLockId> HeldLWLockIds();

}  // namespace pgcpp::storage

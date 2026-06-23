// lock.h — Lock manager for relation-level locks.
//
// Converted from PostgreSQL 15's src/include/storage/lock.h and
// src/backend/storage/lmgr/lock.cpp.
//
// PostgreSQL's lock manager supports both regular locks (table-level,
// acquired by name) and lightweight locks (spinlock-based, for internal
// synchronization). MyToyDB is single-process, so:
//   - No actual blocking (a lock request always succeeds immediately)
//   - Locks are tracked for release at transaction end
//   - The compatibility matrix is preserved for correctness checks
//   - Row-level locks are handled via MVCC (not the lock manager)
//
// Lock modes (weakest to strongest):
//   AccessShareLock        — SELECT
//   RowShareLock           — SELECT FOR UPDATE/SHARE
//   RowExclusiveLock       — INSERT/UPDATE/DELETE
//   ShareUpdateExclusiveLock — VACUUM, ANALYZE
//   ShareLock              — CREATE INDEX
//   ShareRowExclusiveLock  — CREATE TRIGGER
//   ExclusiveLock          — ALTER TABLE
//   AccessExclusiveLock    — DROP, TRUNCATE
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "mytoydb/catalog/catalog.h"  // Oid

namespace mytoydb::transaction {

// LockMode — the strength of a lock.
// Values match PostgreSQL's LockMode enum for compatibility.
enum class LockMode : int {
    kNoLock = 0,                // InvalidLockMode
    kAccessShareLock = 1,       // SELECT
    kRowShareLock = 2,          // SELECT FOR UPDATE/SHARE
    kRowExclusiveLock = 3,      // INSERT/UPDATE/DELETE
    kShareUpdateExclusiveLock = 4, // VACUUM, ANALYZE
    kShareLock = 5,             // CREATE INDEX
    kShareRowExclusiveLock = 6, // CREATE TRIGGER
    kExclusiveLock = 7,         // ALTER TABLE
    kAccessExclusiveLock = 8,   // DROP, TRUNCATE
};

// Number of lock modes (excluding kNoLock).
constexpr int kNumLockModes = 8;

// LockTag — identifies the object being locked.
// In PostgreSQL, this is a 16-byte struct with locktag_type and fields.
// MyToyDB uses a simpler struct for relation locks only.
struct LockTag {
    mytoydb::catalog::Oid relid = 0;  // relation OID
    int locktag_type = 0;              // LOCKTAG_RELATION

    bool operator==(const LockTag&) const = default;
};

// LockTagHash — hash function for LockTag.
struct LockTagHash {
    std::size_t operator()(const LockTag& t) const {
        return std::hash<uint32_t>()(t.relid) ^
               (std::hash<int>()(t.locktag_type) << 1);
    }
};

// Lock tag types (PostgreSQL's LockTagType).
constexpr int kLockTagRelation = 1;
constexpr int kLockTagExtend = 2;
constexpr int kLockTagPage = 3;
constexpr int kLockTagTuple = 4;
constexpr int kLockTagTransaction = 5;

// --- Lock compatibility matrix ---
//
// LockConflicts[mode1][mode2] is true if mode1 conflicts with mode2.
// Two locks conflict if they cannot be held simultaneously by different
// transactions.
//
// The matrix follows PostgreSQL's lock_matrix:
//   - AccessShare conflicts only with AccessExclusive
//   - AccessExclusive conflicts with everything
//   - Higher modes conflict with more modes
bool LockConflicts(LockMode mode1, LockMode mode2);

// --- Lock manager API ---

// LockAcquire — acquire a lock on the given object.
//
// In MyToyDB (single-process), this always succeeds immediately (no
// blocking). The lock is recorded for release at transaction end.
//
// Parameters:
//   tag — the object to lock (relation OID + type)
//   lockmode — the lock mode to acquire
//   session_lock — if true, the lock persists until explicitly released
//                  (not auto-released at transaction end)
//
// Returns true if the lock was acquired.
bool LockAcquire(const LockTag& tag, LockMode lockmode, bool session_lock = false);

// LockRelease — release a lock on the given object.
// Returns true if the lock was found and released.
bool LockRelease(const LockTag& tag, LockMode lockmode);

// LockReleaseAll — release all locks held by the current transaction.
// Called at COMMIT or ABORT.
// If session_lock is false, releases only transaction-level locks.
void LockReleaseAll(bool session_lock = false);

// LockHeld — check if the current transaction holds a lock of at least
// the given mode on the object.
bool LockHeld(const LockTag& tag, LockMode lockmode);

// LockModeHeld — return the strongest lock mode held on the object,
// or kNoLock if none.
LockMode LockModeHeld(const LockTag& tag);

// --- Initialization ---

// InitializeLockManager — set up the lock manager state.
void InitializeLockManager();

// ResetLockManager — release all locks and reset state (for testing).
void ResetLockManager();

// --- Lock count (for testing) ---

// GetLockCount — return the number of locks currently held.
int GetLockCount();

}  // namespace mytoydb::transaction

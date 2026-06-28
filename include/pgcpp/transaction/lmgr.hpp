// lmgr.h — Relation-level lock convenience functions.
//
// Converted from PostgreSQL 15's src/include/storage/lmgr.h.
//
// These wrappers build a LockTag from a relation OID and call the lock
// manager. They are the primary lock API used by the executor and DDL code.
//
// Common usage:
//   LockRelation(relid, AccessShareLock);   // SELECT
//   UnlockRelation(relid, AccessShareLock);
//   LockRelation(relid, RowExclusiveLock);  // INSERT/UPDATE/DELETE
//   LockRelation(relid, AccessExclusiveLock); // DROP/TRUNCATE
#pragma once

#include "mytoydb/catalog/catalog.hpp"  // Oid
#include "mytoydb/transaction/lock.hpp"

namespace mytoydb::transaction {

// LockRelation — acquire a lock on a relation.
// Returns true if the lock was acquired.
bool LockRelation(mytoydb::catalog::Oid relid, LockMode lockmode);

// UnlockRelation — release a lock on a relation.
// Returns true if the lock was released.
bool UnlockRelation(mytoydb::catalog::Oid relid, LockMode lockmode);

// UnlockRelations — release locks on multiple relations.
void UnlockRelations(const std::vector<mytoydb::catalog::Oid>& relids, LockMode lockmode);

// LockRelationIdForSession — acquire a session-level lock (persists
// across transactions). Used by LOCK TABLE and advisory locks.
bool LockRelationIdForSession(mytoydb::catalog::Oid relid, LockMode lockmode);

// UnlockRelationIdForSession — release a session-level lock.
bool UnlockRelationIdForSession(mytoydb::catalog::Oid relid, LockMode lockmode);

// --- Predicate lock stubs (not implemented in MyToyDB) ---
//
// PostgreSQL uses predicate locks for serializable transaction isolation.
// MyToyDB does not implement serializable isolation, so these are no-ops.

inline void PredicateLockRelation(mytoydb::catalog::Oid /*relid*/) {
    // No-op (serializable isolation not supported).
}

// --- Lock mode helpers ---

// LockModeIsSelfExclusive — true if the mode conflicts with itself.
// Used to determine whether a lock can be upgraded.
inline bool LockModeIsSelfExclusive(LockMode mode) {
    return LockConflicts(mode, mode);
}

// LockModeStronger — return the stronger of two lock modes.
inline LockMode LockModeStronger(LockMode a, LockMode b) {
    return static_cast<int>(a) >= static_cast<int>(b) ? a : b;
}

}  // namespace mytoydb::transaction

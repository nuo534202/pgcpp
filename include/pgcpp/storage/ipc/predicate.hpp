// predicate.h — Predicate locks for SERIALIZABLE isolation.
//
// Converted from PostgreSQL 15's src/include/storage/predicate.h and
// src/backend/storage/ipc/predicate.c.
//
// SERIALIZABLE isolation uses predicate locks to detect read/write conflicts
// between transactions at the predicate (tuple / page / relation) granularity.
// PG stores them in a shared-memory hash table keyed by (relation, page, tuple).
//
// MyToyDB is single-process and does not yet implement true SERIALIZABLE
// conflict detection, but the API is preserved so callers can register
// predicate locks and query them. PredicateLockConflicts returns whether a
// given (relation, page, tuple) conflicts with any held predicate lock.
#pragma once

#include <cstdint>
#include <vector>

#include "mytoydb/storage/relfilenode.hpp"

namespace mytoydb::storage {

// PredicateLockTargetTag — identifies what a predicate lock is on.
// Matches PG's PREDICATELOCKTARGETTAG layout (relation + page + tuple).
struct PredicateLockTargetTag {
    RelFileNode rnode;
    uint32_t block_num = 0;   // kInvalidBlockNumber for relation-level
    uint16_t offset_num = 0;  // 0 for page-level

    bool operator==(const PredicateLockTargetTag&) const = default;
};

// PredicateLockTargetHash — hash for use in std::unordered_map.
struct PredicateLockTargetHash {
    std::size_t operator()(const PredicateLockTargetTag& t) const {
        return std::hash<uint32_t>()(t.rnode.spc_node) ^
               (std::hash<uint32_t>()(t.rnode.db_node) << 1) ^
               (std::hash<uint32_t>()(t.rnode.rel_node) << 2) ^
               (std::hash<uint32_t>()(t.block_num) << 3) ^
               (std::hash<uint16_t>()(t.offset_num) << 4);
    }
};

// PredicateLock — a registered predicate lock held by a transaction.
struct PredicateLock {
    PredicateLockTargetTag tag;
    uint32_t xid = 0;    // holder's XID
    bool shared = true;  // PG uses SIReadLock (always "shared" at the lock level)
};

// PredicateLockTuple — acquire a tuple-level predicate lock.
void PredicateLockTuple(const RelFileNode& rnode, uint32_t block_num, uint16_t offset_num,
                        uint32_t xid);

// PredicateLockPage — acquire a page-level predicate lock.
void PredicateLockPage(const RelFileNode& rnode, uint32_t block_num, uint32_t xid);

// PredicateLockRelation — acquire a relation-level predicate lock.
void PredicateLockRelation(const RelFileNode& rnode, uint32_t xid);

// PredicateLockRelease — release all predicate locks held by xid.
int PredicateLockRelease(uint32_t xid);

// PredicateLockReleaseAll — drop all predicate locks (used by tests).
void PredicateLockReleaseAll();

// PredicateLockConflicts — true if any held predicate lock covers the given
// (relation, page, tuple). Page locks cover all tuples on the page;
// relation locks cover all pages and tuples.
bool PredicateLockConflicts(const RelFileNode& rnode, uint32_t block_num, uint16_t offset_num);

// GetPredicateLocks — return a snapshot of all currently-held predicate locks.
std::vector<PredicateLock> GetPredicateLocks();

// NumPredicateLocks — count of currently-held predicate locks.
int NumPredicateLocks();

}  // namespace mytoydb::storage

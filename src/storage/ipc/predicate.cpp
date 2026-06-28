// predicate.cpp — Predicate locks for SERIALIZABLE isolation.
//
// Converted from PostgreSQL 15's src/backend/storage/ipc/predicate.c.
#include "mytoydb/storage/ipc/predicate.hpp"

#include <algorithm>
#include <vector>

#include "mytoydb/storage/block.hpp"

namespace mytoydb::storage {

namespace {

std::vector<PredicateLock>& Locks() {
    static std::vector<PredicateLock> v;
    return v;
}

bool MatchesTag(const PredicateLockTargetTag& lock, const RelFileNode& rnode, uint32_t block_num,
                uint16_t offset_num) {
    if (lock.rnode != rnode) {
        return false;
    }
    // Relation-level lock (block_num == kInvalidBlockNumber): covers everything.
    if (lock.block_num == kInvalidBlockNumber) {
        return true;
    }
    // Page-level lock (offset_num == 0): covers all tuples on the page.
    if (lock.block_num == block_num && lock.offset_num == 0) {
        return true;
    }
    // Tuple-level: exact match.
    return lock.block_num == block_num && lock.offset_num == offset_num;
}

void Acquire(const RelFileNode& rnode, uint32_t block_num, uint16_t offset_num, uint32_t xid) {
    PredicateLock lock;
    lock.tag.rnode = rnode;
    lock.tag.block_num = block_num;
    lock.tag.offset_num = offset_num;
    lock.xid = xid;
    lock.shared = true;
    // De-duplicate: don't add a lock that's already held by this xid for this tag.
    for (const auto& existing : Locks()) {
        if (existing.xid == xid && existing.tag == lock.tag) {
            return;
        }
    }
    Locks().push_back(lock);
}

}  // namespace

void PredicateLockTuple(const RelFileNode& rnode, uint32_t block_num, uint16_t offset_num,
                        uint32_t xid) {
    Acquire(rnode, block_num, offset_num, xid);
}

void PredicateLockPage(const RelFileNode& rnode, uint32_t block_num, uint32_t xid) {
    // Page-level lock uses offset_num == 0.
    Acquire(rnode, block_num, /*offset_num=*/0, xid);
}

void PredicateLockRelation(const RelFileNode& rnode, uint32_t xid) {
    // Relation-level lock uses block_num == kInvalidBlockNumber.
    Acquire(rnode, kInvalidBlockNumber, /*offset_num=*/0, xid);
}

int PredicateLockRelease(uint32_t xid) {
    auto& locks = Locks();
    int removed = 0;
    for (auto it = locks.begin(); it != locks.end();) {
        if (it->xid == xid) {
            it = locks.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

void PredicateLockReleaseAll() {
    Locks().clear();
}

bool PredicateLockConflicts(const RelFileNode& rnode, uint32_t block_num, uint16_t offset_num) {
    for (const auto& lock : Locks()) {
        if (MatchesTag(lock.tag, rnode, block_num, offset_num)) {
            return true;
        }
    }
    return false;
}

std::vector<PredicateLock> GetPredicateLocks() {
    return Locks();
}

int NumPredicateLocks() {
    return static_cast<int>(Locks().size());
}

}  // namespace mytoydb::storage

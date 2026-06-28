// lwlock.cpp — Lightweight reader/writer locks (in-process simulation).
//
// Converted from PostgreSQL 15's src/backend/storage/ipc/lwlock.c.
#include "mytoydb/storage/ipc/lwlock.hpp"

#include <map>
#include <vector>

namespace mytoydb::storage {

namespace {

// HeldLock — records one LWLock currently held by this backend.
struct HeldLock {
    LWLock* lock;
    LWLockMode mode;
    int count;  // recursive acquire count (matches PG's enter/leave count)
};

std::vector<HeldLock>& HeldLocks() {
    static std::vector<HeldLock> v;
    return v;
}

// NamedLocks — the builtin tranche of named LWLocks (one per LWLockId).
std::map<LWLockId, LWLock>& NamedLocks() {
    static std::map<LWLockId, LWLock> m;
    return m;
}

HeldLock* FindHeld(LWLock* lock) {
    for (auto& h : HeldLocks()) {
        if (h.lock == lock) {
            return &h;
        }
    }
    return nullptr;
}

}  // namespace

void LWLockInitialize(LWLock* lock, LWLockId id, int tranche) {
    lock->shared_holders = 0;
    lock->exclusive_held = false;
    lock->id = id;
    lock->tranche = tranche;
}

bool LWLockAcquire(LWLock* lock, LWLockMode mode) {
    if (mode == LWLockMode::kExclusive) {
        // In single-process MyToyDB, exclusive acquire requires no shared
        // holders and no existing exclusive hold.
        if (lock->exclusive_held) {
            // Recursive exclusive acquire by the same backend: bump count.
            if (auto* h = FindHeld(lock); h != nullptr && h->mode == LWLockMode::kExclusive) {
                h->count++;
                return true;
            }
            return false;
        }
        lock->exclusive_held = true;
    } else {
        // Shared acquire requires no exclusive holder.
        if (lock->exclusive_held) {
            if (auto* h = FindHeld(lock); h != nullptr && h->mode == LWLockMode::kExclusive) {
                // Downgrade-from-exclusive isn't supported; bump shared count anyway.
                lock->shared_holders++;
                return true;
            }
            return false;
        }
        lock->shared_holders++;
    }
    HeldLocks().push_back({lock, mode, 1});
    return true;
}

void LWLockRelease(LWLock* lock) {
    auto& held = HeldLocks();
    for (auto it = held.begin(); it != held.end(); ++it) {
        if (it->lock == lock) {
            if (--it->count > 0) {
                return;  // still held recursively
            }
            if (it->mode == LWLockMode::kExclusive) {
                lock->exclusive_held = false;
            } else {
                if (lock->shared_holders > 0) {
                    lock->shared_holders--;
                }
            }
            held.erase(it);
            return;
        }
    }
}

bool LWLockHeldByMe(const LWLock* lock) {
    return FindHeld(const_cast<LWLock*>(lock)) != nullptr;
}

bool LWLockHeldByMeInMode(const LWLock* lock, LWLockMode mode) {
    if (auto* h = FindHeld(const_cast<LWLock*>(lock)); h != nullptr) {
        return h->mode == mode;
    }
    return false;
}

bool LWLockConditionalAcquire(LWLock* lock, LWLockMode mode) {
    if (mode == LWLockMode::kExclusive) {
        if (lock->exclusive_held || lock->shared_holders > 0) {
            return false;
        }
    } else {
        if (lock->exclusive_held) {
            return false;
        }
    }
    return LWLockAcquire(lock, mode);
}

LWLock* LookupNamedLock(LWLockId id) {
    auto& m = NamedLocks();
    auto it = m.find(id);
    if (it == m.end()) {
        LWLock lock;
        LWLockInitialize(&lock, id);
        m[id] = lock;
        return &m[id];
    }
    return &it->second;
}

void InitializeAllLWLocks() {
    // Lazily create all builtin locks; this matches PG's InitLWLocks() that
    // pre-initializes the builtin tranche.
    for (int i = 0; i <= static_cast<int>(LWLockId::kBufferIOCount); ++i) {
        (void)LookupNamedLock(static_cast<LWLockId>(i));
    }
}

void ResetAllLWLocks() {
    NamedLocks().clear();
    HeldLocks().clear();
}

int NumHeldLWLocks() {
    int total = 0;
    for (const auto& h : HeldLocks()) {
        total += h.count;
    }
    return total;
}

std::vector<LWLockId> HeldLWLockIds() {
    std::vector<LWLockId> ids;
    for (const auto& h : HeldLocks()) {
        ids.push_back(h.lock->id);
    }
    return ids;
}

}  // namespace mytoydb::storage

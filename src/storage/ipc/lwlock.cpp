// lwlock.cpp — Lightweight reader/writer locks.
//
// Converted from PostgreSQL 15's src/backend/storage/ipc/lwlock.c.
//
// Uses std::atomic for cross-process lock state. The named lock array is
// allocated in shared memory via ShmemInitStruct so fork'd backends share
// the same lock instances. A spin-then-yield strategy handles contention.
#include "storage/ipc/lwlock.hpp"

#include <sched.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

#include "storage/ipc/shmem.hpp"

namespace pgcpp::storage {

// State word bit layout:
//   bit 31 (0x80000000): exclusive held
//   bits 0-30: shared holder count
constexpr uint32_t kExclusiveBit = 0x80000000u;
constexpr uint32_t kSharedCountMask = 0x7FFFFFFFu;

namespace {

// HeldLock — records one LWLock currently held by this backend.
// This is per-process state (each fork'd backend has its own list).
struct HeldLock {
    LWLock* lock;
    LWLockMode mode;
    int count;  // recursive acquire count
};

std::vector<HeldLock>& HeldLocks() {
    static std::vector<HeldLock> v;
    return v;
}

// Pointer to the named lock array. Allocated in shared memory (or the
// test-mode fallback). Set by InitializeAllLWLocks().
LWLock* g_named_locks = nullptr;

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
    lock->state.store(0, std::memory_order_relaxed);
    lock->id = id;
    lock->tranche = tranche;
}

bool LWLockAcquire(LWLock* lock, LWLockMode mode) {
    // Check for recursive acquire (same backend already holds this lock).
    if (auto* h = FindHeld(lock); h != nullptr) {
        if (h->mode == mode) {
            h->count++;
            return true;
        }
        // Lock is held by me in a different mode — would deadlock.
        // In PG this would be an error; we return false for now.
        return false;
    }

    if (mode == LWLockMode::kExclusive) {
        // Spin until we can set the exclusive bit (state must be 0).
        uint32_t expected = 0;
        while (!lock->state.compare_exchange_weak(
            expected, kExclusiveBit, std::memory_order_acquire, std::memory_order_relaxed)) {
            expected = 0;
            sched_yield();  // give other processes a chance
        }
    } else {
        // Shared: increment the shared count, but only if no exclusive holder.
        uint32_t old_state = lock->state.load(std::memory_order_relaxed);
        while (true) {
            if (old_state & kExclusiveBit) {
                // Exclusive holder exists; spin.
                sched_yield();
                old_state = lock->state.load(std::memory_order_relaxed);
                continue;
            }
            uint32_t new_state = old_state + 1;  // increment shared count
            if (lock->state.compare_exchange_weak(old_state, new_state, std::memory_order_acquire,
                                                  std::memory_order_relaxed)) {
                break;
            }
            // CAS failed, old_state was updated; retry.
        }
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
                lock->state.fetch_and(~kExclusiveBit, std::memory_order_release);
            } else {
                lock->state.fetch_sub(1, std::memory_order_release);
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
    if (auto* h = FindHeld(lock); h != nullptr && h->mode == mode) {
        h->count++;
        return true;
    }

    if (mode == LWLockMode::kExclusive) {
        uint32_t expected = 0;
        if (lock->state.compare_exchange_strong(expected, kExclusiveBit, std::memory_order_acquire,
                                                std::memory_order_relaxed)) {
            HeldLocks().push_back({lock, mode, 1});
            return true;
        }
        return false;
    } else {
        uint32_t old_state = lock->state.load(std::memory_order_relaxed);
        while (!(old_state & kExclusiveBit)) {
            uint32_t new_state = old_state + 1;
            if (lock->state.compare_exchange_weak(old_state, new_state, std::memory_order_acquire,
                                                  std::memory_order_relaxed)) {
                HeldLocks().push_back({lock, mode, 1});
                return true;
            }
        }
        return false;
    }
}

LWLock* LookupNamedLock(LWLockId id) {
    if (g_named_locks == nullptr) {
        InitializeAllLWLocks();
    }
    int idx = static_cast<int>(id);
    if (idx < 0 || idx >= kNumNamedLWLocks) {
        return nullptr;
    }
    return &g_named_locks[idx];
}

void InitializeAllLWLocks() {
    if (g_named_locks != nullptr) {
        return;  // already initialized
    }

    bool found = false;
    g_named_locks = static_cast<LWLock*>(
        ShmemInitStruct("LWLockArray", sizeof(LWLock) * kNumNamedLWLocks, &found));

    if (!found) {
        for (int i = 0; i < kNumNamedLWLocks; ++i) {
            LWLockInitialize(&g_named_locks[i], static_cast<LWLockId>(i));
        }
    }
}

void ResetAllLWLocks() {
    g_named_locks = nullptr;
    HeldLocks().clear();
}

void ResetHeldLWLocks() {
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

}  // namespace pgcpp::storage

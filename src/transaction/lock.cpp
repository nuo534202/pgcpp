// lock.cpp — Lock manager implementation.
//
// Converted from PostgreSQL 15's src/backend/storage/lmgr/lock.cpp.
//
// Implements relation-level locking with the PostgreSQL compatibility
// matrix. In pgcpp (single-process), locks never block — a request
// always succeeds immediately. Locks are tracked for release at
// transaction end (COMMIT/ABORT).
//
// The compatibility matrix determines whether two lock modes can coexist.
// In single-process mode, the same transaction can hold multiple locks on
// the same object (upgrading is allowed). The matrix is preserved for
// correctness and for future multi-process support.
#include "transaction/lock.hpp"

#include "common/error/elog.hpp"

namespace pgcpp::transaction {

namespace {

// Lock compatibility matrix.
// Indexed by [mode1][mode2], true means conflict.
// Modes are 1-based (index 0 = kNoLock, unused).
//
// This matches PostgreSQL's LockConflicts[] bitmask in lock.c:
//   AS  conflicts with: AE
//   RS  conflicts with: E, AE
//   RE  conflicts with: S, SRE, E, AE
//   SUE conflicts with: SUE, S, SRE, E, AE
//   S   conflicts with: RE, SUE, SRE, E, AE
//   SRE conflicts with: RE, SUE, S, SRE, E, AE
//   E   conflicts with: RS, RE, SUE, S, SRE, E, AE
//   AE  conflicts with: AS, RS, RE, SUE, S, SRE, E, AE
const bool kConflictMatrix[kNumLockModes + 1][kNumLockModes + 1] = {
    //              AS     RS     RE     SUE    S      SRE    E      AE
    /* NoLock  */ {false, false, false, false, false, false, false, false, false},
    /* AS      */ {false, false, false, false, false, false, false, false, true},
    /* RS      */ {false, false, false, false, false, false, false, true, true},
    /* RE      */ {false, false, false, false, false, true, true, true, true},
    /* SUE     */ {false, false, false, false, true, true, true, true, true},
    /* S       */ {false, false, false, true, true, false, true, true, true},
    /* SRE     */ {false, false, false, true, true, true, true, true, true},
    /* E       */ {false, false, true, true, true, true, true, true, true},
    /* AE      */ {true, true, true, true, true, true, true, true, true},
};

// A held lock entry.
struct HeldLock {
    LockTag tag;
    LockMode mode;
    bool session_lock;
};

// All locks currently held by the (single) backend.
std::vector<HeldLock>& HeldLocks() {
    static std::vector<HeldLock> locks;
    return locks;
}

// Find the index of a held lock matching (tag, mode), or -1 if not found.
int FindHeldLock(const LockTag& tag, LockMode mode) {
    auto& locks = HeldLocks();
    for (int i = 0; i < static_cast<int>(locks.size()); ++i) {
        if (locks[i].tag == tag && locks[i].mode == mode) {
            return i;
        }
    }
    return -1;
}

}  // namespace

bool LockConflicts(LockMode mode1, LockMode mode2) {
    int m1 = static_cast<int>(mode1);
    int m2 = static_cast<int>(mode2);
    if (m1 < 0 || m1 > kNumLockModes || m2 < 0 || m2 > kNumLockModes) {
        return true;  // unknown mode — be safe
    }
    return kConflictMatrix[m1][m2];
}

bool LockAcquire(const LockTag& tag, LockMode lockmode, bool session_lock) {
    if (lockmode == LockMode::kNoLock) {
        return true;  // no-op
    }

    // Check if we already hold this exact lock.
    if (FindHeldLock(tag, lockmode) >= 0) {
        return true;  // already held
    }

    // In single-process mode, we don't check conflicts (no other backend).
    // Just record the lock.
    HeldLocks().push_back({tag, lockmode, session_lock});
    return true;
}

bool LockRelease(const LockTag& tag, LockMode lockmode) {
    int idx = FindHeldLock(tag, lockmode);
    if (idx < 0) {
        return false;  // not held
    }

    auto& locks = HeldLocks();
    locks.erase(locks.begin() + idx);
    return true;
}

void LockReleaseAll(bool session_lock) {
    auto& locks = HeldLocks();
    if (session_lock) {
        // Release all locks (both session and transaction).
        locks.clear();
    } else {
        // Release only transaction-level locks (not session locks).
        std::vector<HeldLock> remaining;
        for (const auto& lock : locks) {
            if (lock.session_lock) {
                remaining.push_back(lock);
            }
        }
        locks = std::move(remaining);
    }
}

bool LockHeld(const LockTag& tag, LockMode lockmode) {
    // Check if we hold a lock of at least the given mode.
    // In PostgreSQL, a stronger lock implies holding weaker locks.
    auto& locks = HeldLocks();
    for (const auto& lock : locks) {
        if (lock.tag == tag) {
            // Check if the held mode is >= requested mode.
            // Since modes are ordered by strength, we compare the enum values.
            if (static_cast<int>(lock.mode) >= static_cast<int>(lockmode)) {
                return true;
            }
        }
    }
    return false;
}

LockMode LockModeHeld(const LockTag& tag) {
    LockMode strongest = LockMode::kNoLock;
    auto& locks = HeldLocks();
    for (const auto& lock : locks) {
        if (lock.tag == tag) {
            if (static_cast<int>(lock.mode) > static_cast<int>(strongest)) {
                strongest = lock.mode;
            }
        }
    }
    return strongest;
}

void InitializeLockManager() {
    HeldLocks().clear();
}

void ResetLockManager() {
    HeldLocks().clear();
}

int GetLockCount() {
    return static_cast<int>(HeldLocks().size());
}

}  // namespace pgcpp::transaction

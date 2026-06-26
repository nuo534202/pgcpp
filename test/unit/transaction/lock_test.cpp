// lock_test.cpp — Unit tests for the lock manager (M7 Task 7.3).
//
// Tests lock acquisition/release, the compatibility matrix, session vs.
// transaction locks, and relation-level lock helpers.

#include "mytoydb/transaction/lock.hpp"

#include <gtest/gtest.h>

#include "mytoydb/transaction/lmgr.hpp"

using mytoydb::catalog::Oid;
using mytoydb::transaction::GetLockCount;
using mytoydb::transaction::InitializeLockManager;
using mytoydb::transaction::kLockTagRelation;
using mytoydb::transaction::kNumLockModes;
using mytoydb::transaction::LockAcquire;
using mytoydb::transaction::LockConflicts;
using mytoydb::transaction::LockHeld;
using mytoydb::transaction::LockMode;
using mytoydb::transaction::LockModeHeld;
using mytoydb::transaction::LockModeStronger;
using mytoydb::transaction::LockRelation;
using mytoydb::transaction::LockRelationIdForSession;
using mytoydb::transaction::LockRelease;
using mytoydb::transaction::LockReleaseAll;
using mytoydb::transaction::LockTag;
using mytoydb::transaction::ResetLockManager;
using mytoydb::transaction::UnlockRelation;
using mytoydb::transaction::UnlockRelationIdForSession;

namespace {

class LockTest : public ::testing::Test {
protected:
    void SetUp() override {
        ResetLockManager();
        InitializeLockManager();
    }
    void TearDown() override { ResetLockManager(); }

    LockTag MakeTag(Oid relid) {
        LockTag tag;
        tag.relid = relid;
        tag.locktag_type = kLockTagRelation;
        return tag;
    }
};

}  // namespace

// --- Compatibility matrix ---

TEST_F(LockTest, AccessShareConflictsOnlyWithAccessExclusive) {
    EXPECT_FALSE(LockConflicts(LockMode::kAccessShareLock, LockMode::kAccessShareLock));
    EXPECT_FALSE(LockConflicts(LockMode::kAccessShareLock, LockMode::kRowShareLock));
    EXPECT_FALSE(LockConflicts(LockMode::kAccessShareLock, LockMode::kRowExclusiveLock));
    EXPECT_FALSE(LockConflicts(LockMode::kAccessShareLock, LockMode::kShareUpdateExclusiveLock));
    EXPECT_FALSE(LockConflicts(LockMode::kAccessShareLock, LockMode::kShareLock));
    EXPECT_FALSE(LockConflicts(LockMode::kAccessShareLock, LockMode::kShareRowExclusiveLock));
    EXPECT_FALSE(LockConflicts(LockMode::kAccessShareLock, LockMode::kExclusiveLock));
    EXPECT_TRUE(LockConflicts(LockMode::kAccessShareLock, LockMode::kAccessExclusiveLock));
}

TEST_F(LockTest, AccessExclusiveConflictsWithAll) {
    EXPECT_TRUE(LockConflicts(LockMode::kAccessExclusiveLock, LockMode::kAccessShareLock));
    EXPECT_TRUE(LockConflicts(LockMode::kAccessExclusiveLock, LockMode::kRowShareLock));
    EXPECT_TRUE(LockConflicts(LockMode::kAccessExclusiveLock, LockMode::kRowExclusiveLock));
    EXPECT_TRUE(LockConflicts(LockMode::kAccessExclusiveLock, LockMode::kShareUpdateExclusiveLock));
    EXPECT_TRUE(LockConflicts(LockMode::kAccessExclusiveLock, LockMode::kShareLock));
    EXPECT_TRUE(LockConflicts(LockMode::kAccessExclusiveLock, LockMode::kShareRowExclusiveLock));
    EXPECT_TRUE(LockConflicts(LockMode::kAccessExclusiveLock, LockMode::kExclusiveLock));
    EXPECT_TRUE(LockConflicts(LockMode::kAccessExclusiveLock, LockMode::kAccessExclusiveLock));
}

TEST_F(LockTest, RowExclusiveConflictsWithShareAndAbove) {
    EXPECT_FALSE(LockConflicts(LockMode::kRowExclusiveLock, LockMode::kAccessShareLock));
    EXPECT_FALSE(LockConflicts(LockMode::kRowExclusiveLock, LockMode::kRowShareLock));
    EXPECT_FALSE(LockConflicts(LockMode::kRowExclusiveLock, LockMode::kRowExclusiveLock));
    EXPECT_FALSE(LockConflicts(LockMode::kRowExclusiveLock, LockMode::kShareUpdateExclusiveLock));
    EXPECT_TRUE(LockConflicts(LockMode::kRowExclusiveLock, LockMode::kShareLock));
    EXPECT_TRUE(LockConflicts(LockMode::kRowExclusiveLock, LockMode::kShareRowExclusiveLock));
    EXPECT_TRUE(LockConflicts(LockMode::kRowExclusiveLock, LockMode::kExclusiveLock));
    EXPECT_TRUE(LockConflicts(LockMode::kRowExclusiveLock, LockMode::kAccessExclusiveLock));
}

TEST_F(LockTest, ShareLockConflictsWithRowExclusiveAndAbove) {
    EXPECT_FALSE(LockConflicts(LockMode::kShareLock, LockMode::kAccessShareLock));
    EXPECT_FALSE(LockConflicts(LockMode::kShareLock, LockMode::kRowShareLock));
    EXPECT_TRUE(LockConflicts(LockMode::kShareLock, LockMode::kRowExclusiveLock));
    EXPECT_TRUE(LockConflicts(LockMode::kShareLock, LockMode::kShareUpdateExclusiveLock));
    EXPECT_FALSE(LockConflicts(LockMode::kShareLock, LockMode::kShareLock));
    EXPECT_TRUE(LockConflicts(LockMode::kShareLock, LockMode::kShareRowExclusiveLock));
    EXPECT_TRUE(LockConflicts(LockMode::kShareLock, LockMode::kExclusiveLock));
    EXPECT_TRUE(LockConflicts(LockMode::kShareLock, LockMode::kAccessExclusiveLock));
}

TEST_F(LockTest, CompatibilityMatrixIsSymmetric) {
    // The matrix should be symmetric: A conflicts with B iff B conflicts with A.
    for (int i = 1; i <= kNumLockModes; ++i) {
        for (int j = 1; j <= kNumLockModes; ++j) {
            LockMode m1 = static_cast<LockMode>(i);
            LockMode m2 = static_cast<LockMode>(j);
            EXPECT_EQ(LockConflicts(m1, m2), LockConflicts(m2, m1))
                << "Asymmetric conflict at modes " << i << " and " << j;
        }
    }
}

// --- Lock acquisition and release ---

TEST_F(LockTest, LockAcquireSucceeds) {
    LockTag tag = MakeTag(16384);
    EXPECT_TRUE(LockAcquire(tag, LockMode::kAccessShareLock));
    EXPECT_EQ(GetLockCount(), 1);
}

TEST_F(LockTest, LockAcquireSameModeIsNoOp) {
    LockTag tag = MakeTag(16384);
    EXPECT_TRUE(LockAcquire(tag, LockMode::kAccessShareLock));
    EXPECT_TRUE(LockAcquire(tag, LockMode::kAccessShareLock));
    EXPECT_EQ(GetLockCount(), 1);
}

TEST_F(LockTest, LockAcquireDifferentModesAreSeparate) {
    LockTag tag = MakeTag(16384);
    EXPECT_TRUE(LockAcquire(tag, LockMode::kAccessShareLock));
    EXPECT_TRUE(LockAcquire(tag, LockMode::kRowShareLock));
    EXPECT_EQ(GetLockCount(), 2);
}

TEST_F(LockTest, LockReleaseRemovesLock) {
    LockTag tag = MakeTag(16384);
    LockAcquire(tag, LockMode::kAccessShareLock);
    EXPECT_TRUE(LockRelease(tag, LockMode::kAccessShareLock));
    EXPECT_EQ(GetLockCount(), 0);
}

TEST_F(LockTest, LockReleaseUnheldReturnsFalse) {
    LockTag tag = MakeTag(16384);
    EXPECT_FALSE(LockRelease(tag, LockMode::kAccessShareLock));
}

TEST_F(LockTest, LockReleaseWrongModeDoesNotRemove) {
    LockTag tag = MakeTag(16384);
    LockAcquire(tag, LockMode::kAccessShareLock);
    EXPECT_FALSE(LockRelease(tag, LockMode::kRowShareLock));
    EXPECT_EQ(GetLockCount(), 1);
}

TEST_F(LockTest, LockReleaseAllClearsTransactionLocks) {
    LockTag tag1 = MakeTag(16384);
    LockTag tag2 = MakeTag(16385);
    LockAcquire(tag1, LockMode::kAccessShareLock);
    LockAcquire(tag2, LockMode::kRowExclusiveLock);
    EXPECT_EQ(GetLockCount(), 2);

    LockReleaseAll(false);
    EXPECT_EQ(GetLockCount(), 0);
}

TEST_F(LockTest, SessionLocksSurviveTransactionEnd) {
    LockTag tag = MakeTag(16384);
    LockAcquire(tag, LockMode::kAccessShareLock, true);  // session lock
    LockAcquire(tag, LockMode::kRowShareLock, false);    // transaction lock
    EXPECT_EQ(GetLockCount(), 2);

    LockReleaseAll(false);  // release transaction locks only
    EXPECT_EQ(GetLockCount(), 1);

    LockReleaseAll(true);  // release all
    EXPECT_EQ(GetLockCount(), 0);
}

// --- LockHeld and LockModeHeld ---

TEST_F(LockTest, LockHeldReturnsTrueForHeldMode) {
    LockTag tag = MakeTag(16384);
    LockAcquire(tag, LockMode::kAccessShareLock);
    EXPECT_TRUE(LockHeld(tag, LockMode::kAccessShareLock));
}

TEST_F(LockTest, LockHeldReturnsFalseForUnheldMode) {
    LockTag tag = MakeTag(16384);
    LockAcquire(tag, LockMode::kAccessShareLock);
    EXPECT_FALSE(LockHeld(tag, LockMode::kRowShareLock));
}

TEST_F(LockTest, LockHeldReturnsTrueForStrongerMode) {
    LockTag tag = MakeTag(16384);
    LockAcquire(tag, LockMode::kAccessExclusiveLock);
    // Holding AccessExclusive implies holding all weaker modes.
    EXPECT_TRUE(LockHeld(tag, LockMode::kAccessShareLock));
    EXPECT_TRUE(LockHeld(tag, LockMode::kRowExclusiveLock));
    EXPECT_TRUE(LockHeld(tag, LockMode::kExclusiveLock));
}

TEST_F(LockTest, LockModeHeldReturnsStrongest) {
    LockTag tag = MakeTag(16384);
    LockAcquire(tag, LockMode::kAccessShareLock);
    LockAcquire(tag, LockMode::kRowExclusiveLock);
    EXPECT_EQ(LockModeHeld(tag), LockMode::kRowExclusiveLock);
}

TEST_F(LockTest, LockModeHeldReturnsNoLockForUnlocked) {
    LockTag tag = MakeTag(16384);
    EXPECT_EQ(LockModeHeld(tag), LockMode::kNoLock);
}

// --- Relation-level helpers ---

TEST_F(LockTest, LockRelationAcquiresLock) {
    EXPECT_TRUE(LockRelation(16384, LockMode::kAccessShareLock));
    EXPECT_EQ(GetLockCount(), 1);
    EXPECT_TRUE(LockHeld(MakeTag(16384), LockMode::kAccessShareLock));
}

TEST_F(LockTest, UnlockRelationReleasesLock) {
    LockRelation(16384, LockMode::kAccessShareLock);
    EXPECT_TRUE(UnlockRelation(16384, LockMode::kAccessShareLock));
    EXPECT_EQ(GetLockCount(), 0);
    EXPECT_FALSE(LockHeld(MakeTag(16384), LockMode::kAccessShareLock));
}

TEST_F(LockTest, SessionLockPersistsAcrossReleaseAll) {
    LockRelationIdForSession(16384, LockMode::kAccessShareLock);
    LockReleaseAll(false);  // transaction end
    EXPECT_EQ(GetLockCount(), 1);
    EXPECT_TRUE(UnlockRelationIdForSession(16384, LockMode::kAccessShareLock));
    EXPECT_EQ(GetLockCount(), 0);
}

// --- LockModeStronger helper ---

TEST_F(LockTest, LockModeStrongerReturnsStronger) {
    EXPECT_EQ(LockModeStronger(LockMode::kAccessShareLock, LockMode::kRowShareLock),
              LockMode::kRowShareLock);
    EXPECT_EQ(LockModeStronger(LockMode::kExclusiveLock, LockMode::kAccessShareLock),
              LockMode::kExclusiveLock);
    EXPECT_EQ(LockModeStronger(LockMode::kShareLock, LockMode::kShareLock), LockMode::kShareLock);
}

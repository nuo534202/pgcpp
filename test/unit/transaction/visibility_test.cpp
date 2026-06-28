// visibility_test.cpp — Unit tests for MVCC visibility (M7 Task 7.2).
//
// Tests HeapTupleSatisfiesMVCC with various xmin/xmax combinations:
//   - Tuple inserted by committed transaction (visible)
//   - Tuple inserted by aborted transaction (invisible)
//   - Tuple inserted by in-progress transaction (invisible)
//   - Tuple deleted by committed transaction (invisible)
//   - Tuple deleted by aborted transaction (visible)
//   - Tuple inserted/deleted by current transaction (depends on CID)
//   - Hint flag setting and reuse

#include "pgcpp/transaction/visibility.hpp"

#include <gtest/gtest.h>

#include <vector>

#include "pgcpp/transaction/heap_tuple.hpp"
#include "pgcpp/transaction/snapshot.hpp"
#include "pgcpp/transaction/transam.hpp"
#include "pgcpp/transaction/xact.hpp"

using mytoydb::transaction::AllocateNextTransactionId;
using mytoydb::transaction::CommandId;
using mytoydb::transaction::GetNextTransactionId;
using mytoydb::transaction::HeapTupleHeaderData;
using mytoydb::transaction::HeapTupleHeaderGetXmaxStatus;
using mytoydb::transaction::HeapTupleHeaderGetXminStatus;
using mytoydb::transaction::HeapTupleHeaderSetCid;
using mytoydb::transaction::HeapTupleHeaderSetXmax;
using mytoydb::transaction::HeapTupleHeaderSetXmin;
using mytoydb::transaction::HeapTupleHeaderSetXminCommitted;
using mytoydb::transaction::HeapTupleHeaderSetXminInvalid;
using mytoydb::transaction::HeapTupleIsSurelyDead;
using mytoydb::transaction::HeapTupleSatisfiesMVCC;
using mytoydb::transaction::InitializeTransactionSystem;
using mytoydb::transaction::kFirstCommandId;
using mytoydb::transaction::kFirstNormalTransactionId;
using mytoydb::transaction::kHeapXmaxCommitted;
using mytoydb::transaction::kHeapXmaxInvalid;
using mytoydb::transaction::kHeapXminCommitted;
using mytoydb::transaction::kHeapXminFrozen;
using mytoydb::transaction::kHeapXminInvalid;
using mytoydb::transaction::kInvalidTransactionId;
using mytoydb::transaction::MakeSnapshot;
using mytoydb::transaction::ResetTransactionState;
using mytoydb::transaction::SnapshotData;
using mytoydb::transaction::SnapshotType;
using mytoydb::transaction::TransactionId;
using mytoydb::transaction::TransactionIdAbort;
using mytoydb::transaction::TransactionIdCommit;
using mytoydb::transaction::TransactionIdGetStatus;
using mytoydb::transaction::XidStatus;
using mytoydb::transaction::XidVisibleInSnapshot;

namespace {

class VisibilityTest : public ::testing::Test {
protected:
    void SetUp() override {
        ResetTransactionState();
        InitializeTransactionSystem();
    }
    void TearDown() override {
        ResetTransactionState();
        InitializeTransactionSystem();
    }

    // Helper: create a tuple header with the given xmin/xmax/cid.
    HeapTupleHeaderData MakeTuple(TransactionId xmin, TransactionId xmax, CommandId cid = 0) {
        HeapTupleHeaderData tup{};
        HeapTupleHeaderSetXmin(&tup, xmin);
        HeapTupleHeaderSetXmax(&tup, xmax);
        HeapTupleHeaderSetCid(&tup, cid);
        return tup;
    }

    // Helper: commit a transaction and return its XID.
    TransactionId CommitXid() {
        TransactionId xid = AllocateNextTransactionId();
        TransactionIdCommit(xid);
        return xid;
    }

    // Helper: abort a transaction and return its XID.
    TransactionId AbortXid() {
        TransactionId xid = AllocateNextTransactionId();
        TransactionIdAbort(xid);
        return xid;
    }

    // Helper: allocate an in-progress XID (not committed or aborted).
    TransactionId InProgressXid() { return AllocateNextTransactionId(); }
};

}  // namespace

// --- Basic visibility scenarios ---

TEST_F(VisibilityTest, TupleInsertedByCommittedTransactionIsVisible) {
    TransactionId xmin = CommitXid();
    auto tup = MakeTuple(xmin, kInvalidTransactionId);

    // Snapshot taken after the commit.
    SnapshotData snap = MakeSnapshot(xmin + 1, xmin + 2);
    EXPECT_TRUE(HeapTupleSatisfiesMVCC(&tup, snap));
}

TEST_F(VisibilityTest, TupleInsertedByAbortedTransactionIsInvisible) {
    TransactionId xmin = AbortXid();
    auto tup = MakeTuple(xmin, kInvalidTransactionId);

    SnapshotData snap = MakeSnapshot(xmin + 1, xmin + 2);
    EXPECT_FALSE(HeapTupleSatisfiesMVCC(&tup, snap));
}

TEST_F(VisibilityTest, TupleInsertedByInProgressTransactionIsInvisible) {
    TransactionId xmin = InProgressXid();
    auto tup = MakeTuple(xmin, kInvalidTransactionId);

    // Snapshot with xmin in the in-progress list.
    SnapshotData snap = MakeSnapshot(xmin, xmin + 1, {xmin});
    EXPECT_FALSE(HeapTupleSatisfiesMVCC(&tup, snap));
}

TEST_F(VisibilityTest, TupleInsertedByFutureTransactionIsInvisible) {
    TransactionId xmin = CommitXid();
    auto tup = MakeTuple(xmin, kInvalidTransactionId);

    // Snapshot taken before the commit (xmax <= xmin).
    SnapshotData snap = MakeSnapshot(kFirstNormalTransactionId, xmin);
    EXPECT_FALSE(HeapTupleSatisfiesMVCC(&tup, snap));
}

// --- Deletion visibility ---

TEST_F(VisibilityTest, TupleDeletedByCommittedTransactionIsInvisible) {
    TransactionId xmin = CommitXid();
    TransactionId xmax = CommitXid();
    auto tup = MakeTuple(xmin, xmax);

    // Snapshot taken after both commit.
    SnapshotData snap = MakeSnapshot(xmax + 1, xmax + 2);
    EXPECT_FALSE(HeapTupleSatisfiesMVCC(&tup, snap));
}

TEST_F(VisibilityTest, TupleDeletedByAbortedTransactionIsVisible) {
    TransactionId xmin = CommitXid();
    TransactionId xmax = AbortXid();
    auto tup = MakeTuple(xmin, xmax);

    SnapshotData snap = MakeSnapshot(xmax + 1, xmax + 2);
    EXPECT_TRUE(HeapTupleSatisfiesMVCC(&tup, snap));
}

TEST_F(VisibilityTest, TupleDeletedByInProgressTransactionIsVisible) {
    TransactionId xmin = CommitXid();
    TransactionId xmax = InProgressXid();
    auto tup = MakeTuple(xmin, xmax);

    // xmax is in-progress at snapshot time.
    SnapshotData snap = MakeSnapshot(xmin, xmax + 1, {xmax});
    EXPECT_TRUE(HeapTupleSatisfiesMVCC(&tup, snap));
}

TEST_F(VisibilityTest, TupleDeletedByFutureTransactionIsVisible) {
    TransactionId xmin = CommitXid();
    TransactionId xmax = CommitXid();
    auto tup = MakeTuple(xmin, xmax);

    // Snapshot taken after xmin but before xmax.
    SnapshotData snap = MakeSnapshot(xmin + 1, xmax);
    EXPECT_TRUE(HeapTupleSatisfiesMVCC(&tup, snap));
}

// --- Hint flags ---

TEST_F(VisibilityTest, HintFlagsSetForCommittedXmin) {
    TransactionId xmin = CommitXid();
    auto tup = MakeTuple(xmin, kInvalidTransactionId);

    // Initially no hint flags.
    EXPECT_EQ(HeapTupleHeaderGetXminStatus(&tup), mytoydb::transaction::XactStatus::kInProgress);

    SnapshotData snap = MakeSnapshot(xmin + 1, xmin + 2);
    HeapTupleSatisfiesMVCC(&tup, snap);

    // After visibility check, the committed hint should be set.
    EXPECT_EQ(HeapTupleHeaderGetXminStatus(&tup), mytoydb::transaction::XactStatus::kCommitted);
}

TEST_F(VisibilityTest, HintFlagsSetForAbortedXmin) {
    TransactionId xmin = AbortXid();
    auto tup = MakeTuple(xmin, kInvalidTransactionId);

    SnapshotData snap = MakeSnapshot(xmin + 1, xmin + 2);
    HeapTupleSatisfiesMVCC(&tup, snap);

    // After visibility check, the invalid (aborted) hint should be set.
    EXPECT_EQ(HeapTupleHeaderGetXminStatus(&tup), mytoydb::transaction::XactStatus::kAborted);
}

TEST_F(VisibilityTest, PreSetCommittedHintMakesVisible) {
    TransactionId xmin = CommitXid();
    auto tup = MakeTuple(xmin, kInvalidTransactionId);

    // Set the committed hint manually.
    HeapTupleHeaderSetXminCommitted(&tup);

    // Even with a snapshot where xmin would be in-progress, the hint
    // short-circuits the check.
    SnapshotData snap = MakeSnapshot(xmin, xmin + 1, {xmin});
    EXPECT_TRUE(HeapTupleSatisfiesMVCC(&tup, snap));
}

TEST_F(VisibilityTest, PreSetInvalidHintMakesInvisible) {
    TransactionId xmin = CommitXid();
    auto tup = MakeTuple(xmin, kInvalidTransactionId);

    // Set the invalid (aborted) hint manually.
    HeapTupleHeaderSetXminInvalid(&tup);

    // Even though xmin is actually committed, the hint says aborted.
    SnapshotData snap = MakeSnapshot(xmin + 1, xmin + 2);
    EXPECT_FALSE(HeapTupleSatisfiesMVCC(&tup, snap));
}

// --- Frozen XID ---

TEST_F(VisibilityTest, FrozenTupleIsAlwaysVisible) {
    auto tup = MakeTuple(mytoydb::transaction::kFrozenTransactionId, kInvalidTransactionId);
    tup.t_infomask |= kHeapXminFrozen;

    SnapshotData snap = MakeSnapshot(kFirstNormalTransactionId, kFirstNormalTransactionId + 1);
    EXPECT_TRUE(HeapTupleSatisfiesMVCC(&tup, snap));
}

// --- XidVisibleInSnapshot helper ---

TEST_F(VisibilityTest, XidVisibleInSnapshotForCommitted) {
    TransactionId xid = CommitXid();
    SnapshotData snap = MakeSnapshot(xid + 1, xid + 2);
    bool hint = false;
    EXPECT_TRUE(XidVisibleInSnapshot(xid, snap, &hint));
    EXPECT_TRUE(hint);
}

TEST_F(VisibilityTest, XidVisibleInSnapshotForAborted) {
    TransactionId xid = AbortXid();
    SnapshotData snap = MakeSnapshot(xid + 1, xid + 2);
    bool hint = false;
    EXPECT_FALSE(XidVisibleInSnapshot(xid, snap, &hint));
    EXPECT_FALSE(hint);
}

TEST_F(VisibilityTest, XidVisibleInSnapshotForInProgress) {
    TransactionId xid = InProgressXid();
    SnapshotData snap = MakeSnapshot(xid, xid + 1, {xid});
    bool hint = false;
    EXPECT_FALSE(XidVisibleInSnapshot(xid, snap, &hint));
    EXPECT_FALSE(hint);
}

TEST_F(VisibilityTest, XidVisibleInSnapshotForFuture) {
    TransactionId xid = CommitXid();
    SnapshotData snap = MakeSnapshot(kFirstNormalTransactionId, xid);
    bool hint = false;
    EXPECT_FALSE(XidVisibleInSnapshot(xid, snap, &hint));
}

// --- HeapTupleIsSurelyDead ---

TEST_F(VisibilityTest, SurelyDeadForCommittedOldDeletion) {
    TransactionId xmin = CommitXid();
    TransactionId xmax = CommitXid();
    auto tup = MakeTuple(xmin, xmax);

    // Snapshot with xmin > xmax (deletion is old).
    SnapshotData snap = MakeSnapshot(xmax + 10, xmax + 11);
    EXPECT_TRUE(HeapTupleIsSurelyDead(&tup, snap));
}

TEST_F(VisibilityTest, NotSurelyDeadForAbortedDeletion) {
    TransactionId xmin = CommitXid();
    TransactionId xmax = AbortXid();
    auto tup = MakeTuple(xmin, xmax);

    SnapshotData snap = MakeSnapshot(xmax + 10, xmax + 11);
    EXPECT_FALSE(HeapTupleIsSurelyDead(&tup, snap));
}

TEST_F(VisibilityTest, NotSurelyDeadForRecentDeletion) {
    TransactionId xmin = CommitXid();
    TransactionId xmax = CommitXid();
    auto tup = MakeTuple(xmin, xmax);

    // Snapshot with xmin <= xmax (deletion is recent).
    SnapshotData snap = MakeSnapshot(xmax, xmax + 1);
    EXPECT_FALSE(HeapTupleIsSurelyDead(&tup, snap));
}

TEST_F(VisibilityTest, NotSurelyDeadForUndeletedTuple) {
    TransactionId xmin = CommitXid();
    auto tup = MakeTuple(xmin, kInvalidTransactionId);

    SnapshotData snap = MakeSnapshot(xmin + 10, xmin + 11);
    EXPECT_FALSE(HeapTupleIsSurelyDead(&tup, snap));
}

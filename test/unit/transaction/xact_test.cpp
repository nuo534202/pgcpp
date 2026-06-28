// xact_test.cpp — Unit tests for the transaction state machine (M7 Task 7.1).
//
// Tests TransactionId assignment, commit log status tracking, the
// transaction state machine (BEGIN/COMMIT/ROLLBACK), savepoints, and
// command counter increment.

#include "pgcpp/transaction/xact.hpp"

#include <gtest/gtest.h>

#include "pgcpp/transaction/transam.hpp"

using pgcpp::transaction::AbortCurrentTransaction;
using pgcpp::transaction::AbortTransactionBlock;
using pgcpp::transaction::AllocateNextTransactionId;
using pgcpp::transaction::BeginSavepoint;
using pgcpp::transaction::BeginTransactionBlock;
using pgcpp::transaction::CommandCounterIncrement;
using pgcpp::transaction::CommandId;
using pgcpp::transaction::CommitTransactionCommand;
using pgcpp::transaction::EndTransactionBlock;
using pgcpp::transaction::GetCurrentCommandId;
using pgcpp::transaction::GetCurrentSubTransactionId;
using pgcpp::transaction::GetCurrentTransactionId;
using pgcpp::transaction::GetCurrentTransactionIdIfAny;
using pgcpp::transaction::GetCurrentTransactionNestingLevel;
using pgcpp::transaction::GetNextTransactionId;
using pgcpp::transaction::InitializeCommitLog;
using pgcpp::transaction::InitializeTransactionSystem;
using pgcpp::transaction::IsAbortedTransactionBlock;
using pgcpp::transaction::IsTransactionBlock;
using pgcpp::transaction::IsTransactionOrTransactionBlock;
using pgcpp::transaction::kBootstrapTransactionId;
using pgcpp::transaction::kFirstCommandId;
using pgcpp::transaction::kFirstNormalTransactionId;
using pgcpp::transaction::kFrozenTransactionId;
using pgcpp::transaction::kInvalidSubTransactionId;
using pgcpp::transaction::kInvalidTransactionId;
using pgcpp::transaction::kTopSubTransactionId;
using pgcpp::transaction::ReleaseSavepoint;
using pgcpp::transaction::ResetTransactionState;
using pgcpp::transaction::RollbackToSavepoint;
using pgcpp::transaction::StartTransactionCommand;
using pgcpp::transaction::SubTransactionId;
using pgcpp::transaction::TBlockState;
using pgcpp::transaction::TransactionBlockStateAsString;
using pgcpp::transaction::TransactionId;
using pgcpp::transaction::TransactionIdAbort;
using pgcpp::transaction::TransactionIdCommit;
using pgcpp::transaction::TransactionIdDidAbort;
using pgcpp::transaction::TransactionIdDidCommit;
using pgcpp::transaction::TransactionIdEquals;
using pgcpp::transaction::TransactionIdFollows;
using pgcpp::transaction::TransactionIdGetStatus;
using pgcpp::transaction::TransactionIdIsNormal;
using pgcpp::transaction::TransactionIdIsValid;
using pgcpp::transaction::TransactionIdPrecedes;
using pgcpp::transaction::TransState;
using pgcpp::transaction::XidStatus;

namespace {

class XactTest : public ::testing::Test {
protected:
    void SetUp() override {
        ResetTransactionState();
        InitializeTransactionSystem();
    }
    void TearDown() override {
        ResetTransactionState();
        InitializeTransactionSystem();
    }
};

}  // namespace

// --- TransactionId constants and predicates ---

TEST_F(XactTest, SpecialTransactionIdsHaveCorrectValues) {
    EXPECT_EQ(kInvalidTransactionId, 0u);
    EXPECT_EQ(kBootstrapTransactionId, 1u);
    EXPECT_EQ(kFrozenTransactionId, 2u);
    EXPECT_EQ(kFirstNormalTransactionId, 3u);
}

TEST_F(XactTest, TransactionIdIsValid) {
    EXPECT_FALSE(TransactionIdIsValid(kInvalidTransactionId));
    EXPECT_TRUE(TransactionIdIsValid(kBootstrapTransactionId));
    EXPECT_TRUE(TransactionIdIsValid(kFrozenTransactionId));
    EXPECT_TRUE(TransactionIdIsValid(kFirstNormalTransactionId));
    EXPECT_TRUE(TransactionIdIsValid(100));
}

TEST_F(XactTest, TransactionIdIsNormal) {
    EXPECT_FALSE(TransactionIdIsNormal(kInvalidTransactionId));
    EXPECT_FALSE(TransactionIdIsNormal(kBootstrapTransactionId));
    EXPECT_FALSE(TransactionIdIsNormal(kFrozenTransactionId));
    EXPECT_TRUE(TransactionIdIsNormal(kFirstNormalTransactionId));
    EXPECT_TRUE(TransactionIdIsNormal(100));
}

// --- Modular comparison ---

TEST_F(XactTest, TransactionIdPrecedes) {
    EXPECT_TRUE(TransactionIdPrecedes(3, 4));
    EXPECT_FALSE(TransactionIdPrecedes(4, 3));
    EXPECT_FALSE(TransactionIdPrecedes(3, 3));
    // Wraparound: a large XID precedes a small XID if the difference is < 2^31
    EXPECT_TRUE(TransactionIdPrecedes(0xFFFFFFFF, 1));
    EXPECT_FALSE(TransactionIdPrecedes(1, 0xFFFFFFFF));
}

TEST_F(XactTest, TransactionIdFollows) {
    EXPECT_TRUE(TransactionIdFollows(4, 3));
    EXPECT_FALSE(TransactionIdFollows(3, 4));
    EXPECT_FALSE(TransactionIdFollows(3, 3));
    EXPECT_TRUE(TransactionIdFollows(1, 0xFFFFFFFF));
}

TEST_F(XactTest, TransactionIdEquals) {
    EXPECT_TRUE(TransactionIdEquals(3, 3));
    EXPECT_FALSE(TransactionIdEquals(3, 4));
}

// --- Commit log ---

TEST_F(XactTest, AllocateNextTransactionIdStartsAtFirstNormal) {
    TransactionId xid = AllocateNextTransactionId();
    EXPECT_EQ(xid, kFirstNormalTransactionId);
    EXPECT_EQ(GetNextTransactionId(), kFirstNormalTransactionId);
}

TEST_F(XactTest, AllocateNextTransactionIdIncrements) {
    TransactionId xid1 = AllocateNextTransactionId();
    TransactionId xid2 = AllocateNextTransactionId();
    TransactionId xid3 = AllocateNextTransactionId();
    EXPECT_EQ(xid1, kFirstNormalTransactionId);
    EXPECT_EQ(xid2, kFirstNormalTransactionId + 1);
    EXPECT_EQ(xid3, kFirstNormalTransactionId + 2);
}

TEST_F(XactTest, NewTransactionIdIsInProgress) {
    TransactionId xid = AllocateNextTransactionId();
    EXPECT_EQ(TransactionIdGetStatus(xid), XidStatus::kInProgress);
}

TEST_F(XactTest, TransactionIdCommitSetsStatus) {
    TransactionId xid = AllocateNextTransactionId();
    TransactionIdCommit(xid);
    EXPECT_EQ(TransactionIdGetStatus(xid), XidStatus::kCommitted);
    EXPECT_TRUE(TransactionIdDidCommit(xid));
    EXPECT_FALSE(TransactionIdDidAbort(xid));
}

TEST_F(XactTest, TransactionIdAbortSetsStatus) {
    TransactionId xid = AllocateNextTransactionId();
    TransactionIdAbort(xid);
    EXPECT_EQ(TransactionIdGetStatus(xid), XidStatus::kAborted);
    EXPECT_FALSE(TransactionIdDidCommit(xid));
    EXPECT_TRUE(TransactionIdDidAbort(xid));
}

TEST_F(XactTest, SpecialXidsAreCommitted) {
    EXPECT_TRUE(TransactionIdDidCommit(kBootstrapTransactionId));
    EXPECT_TRUE(TransactionIdDidCommit(kFrozenTransactionId));
    EXPECT_FALSE(TransactionIdDidAbort(kBootstrapTransactionId));
    EXPECT_FALSE(TransactionIdDidAbort(kFrozenTransactionId));
}

TEST_F(XactTest, UnassignedXidIsInProgress) {
    // An XID that was never allocated is treated as in-progress.
    TransactionId xid = 99999;
    EXPECT_EQ(TransactionIdGetStatus(xid), XidStatus::kInProgress);
    EXPECT_FALSE(TransactionIdDidCommit(xid));
}

// --- Transaction state machine ---

TEST_F(XactTest, NoTransactionActiveInitially) {
    EXPECT_FALSE(IsTransactionOrTransactionBlock());
    EXPECT_FALSE(IsTransactionBlock());
    EXPECT_EQ(GetCurrentTransactionIdIfAny(), kInvalidTransactionId);
    EXPECT_EQ(GetCurrentTransactionNestingLevel(), 0);
}

TEST_F(XactTest, StartTransactionCommandBeginsAutocommit) {
    StartTransactionCommand();
    EXPECT_TRUE(IsTransactionOrTransactionBlock());
    EXPECT_FALSE(IsTransactionBlock());
    EXPECT_EQ(GetCurrentTransactionNestingLevel(), 1);
    CommitTransactionCommand();
    EXPECT_FALSE(IsTransactionOrTransactionBlock());
}

TEST_F(XactTest, GetCurrentTransactionIdDefersAllocation) {
    StartTransactionCommand();
    // Before calling GetCurrentTransactionId, no XID is assigned.
    EXPECT_EQ(GetCurrentTransactionIdIfAny(), kInvalidTransactionId);

    // Calling GetCurrentTransactionId allocates an XID.
    TransactionId xid = GetCurrentTransactionId();
    EXPECT_EQ(xid, kFirstNormalTransactionId);
    EXPECT_EQ(GetCurrentTransactionIdIfAny(), xid);

    // Subsequent calls return the same XID.
    EXPECT_EQ(GetCurrentTransactionId(), xid);

    CommitTransactionCommand();
}

TEST_F(XactTest, BeginTransactionBlockStartsExplicitTransaction) {
    EXPECT_TRUE(BeginTransactionBlock());
    EXPECT_TRUE(IsTransactionBlock());
    EXPECT_TRUE(IsTransactionOrTransactionBlock());
    EXPECT_EQ(GetCurrentTransactionNestingLevel(), 1);
}

TEST_F(XactTest, EndTransactionBlockCommits) {
    BeginTransactionBlock();
    TransactionId xid = GetCurrentTransactionId();
    EXPECT_TRUE(EndTransactionBlock());
    EXPECT_FALSE(IsTransactionBlock());
    EXPECT_FALSE(IsTransactionOrTransactionBlock());
    // The XID should be recorded as committed.
    EXPECT_TRUE(TransactionIdDidCommit(xid));
}

TEST_F(XactTest, AbortTransactionBlockRollsBack) {
    BeginTransactionBlock();
    TransactionId xid = GetCurrentTransactionId();
    AbortTransactionBlock();
    EXPECT_FALSE(IsTransactionBlock());
    EXPECT_FALSE(IsTransactionOrTransactionBlock());
    // The XID should be recorded as aborted.
    EXPECT_TRUE(TransactionIdDidAbort(xid));
}

TEST_F(XactTest, CommandCounterIncrementAdvancesCid) {
    StartTransactionCommand();
    CommandId cid1 = GetCurrentCommandId(false);
    EXPECT_EQ(cid1, kFirstCommandId);

    CommandCounterIncrement();
    CommandId cid2 = GetCurrentCommandId(false);
    EXPECT_EQ(cid2, kFirstCommandId + 1);

    CommandCounterIncrement();
    CommandId cid3 = GetCurrentCommandId(false);
    EXPECT_EQ(cid3, kFirstCommandId + 2);

    CommitTransactionCommand();
}

TEST_F(XactTest, CommitTransactionCommandInBlockIncrementsCid) {
    BeginTransactionBlock();
    CommandId cid1 = GetCurrentCommandId(false);
    CommitTransactionCommand();  // Inside a block, just increments CID.
    CommandId cid2 = GetCurrentCommandId(false);
    EXPECT_EQ(cid2, cid1 + 1);
    EXPECT_TRUE(IsTransactionBlock());  // Still in the block.
    EndTransactionBlock();
}

TEST_F(XactTest, AbortCurrentTransactionInAutocommit) {
    StartTransactionCommand();
    TransactionId xid = GetCurrentTransactionId();
    AbortCurrentTransaction();
    EXPECT_FALSE(IsTransactionOrTransactionBlock());
    EXPECT_TRUE(TransactionIdDidAbort(xid));
}

TEST_F(XactTest, MultipleTransactionsGetSequentialXids) {
    StartTransactionCommand();
    TransactionId xid1 = GetCurrentTransactionId();
    CommitTransactionCommand();

    StartTransactionCommand();
    TransactionId xid2 = GetCurrentTransactionId();
    CommitTransactionCommand();

    EXPECT_EQ(xid2, xid1 + 1);
}

// --- Savepoints ---

TEST_F(XactTest, BeginSavepointCreatesSubtransaction) {
    BeginTransactionBlock();
    int level1 = GetCurrentTransactionNestingLevel();
    BeginSavepoint("sp1");
    int level2 = GetCurrentTransactionNestingLevel();
    EXPECT_EQ(level2, level1 + 1);
    EXPECT_EQ(GetCurrentSubTransactionId(), kTopSubTransactionId + 1);
}

TEST_F(XactTest, ReleaseSavepointCommitsSubtransaction) {
    BeginTransactionBlock();
    BeginSavepoint("sp1");
    CommandId cid_before = GetCurrentCommandId(false);
    CommandCounterIncrement();
    ReleaseSavepoint("sp1");

    // After release, we're back at the top-level transaction.
    EXPECT_EQ(GetCurrentTransactionNestingLevel(), 1);
    // The command ID should be propagated to the parent.
    EXPECT_EQ(GetCurrentCommandId(false), cid_before + 1);
    EndTransactionBlock();
}

TEST_F(XactTest, RollbackToSavepointRestoresCommandId) {
    BeginTransactionBlock();
    CommandId cid_before = GetCurrentCommandId(false);
    BeginSavepoint("sp1");
    CommandCounterIncrement();
    CommandCounterIncrement();
    // CID should be advanced inside the subtransaction.
    EXPECT_EQ(GetCurrentCommandId(false), cid_before + 2);

    RollbackToSavepoint("sp1");
    // After rollback, CID should be restored to the value before the subxact.
    EXPECT_EQ(GetCurrentCommandId(false), cid_before);
    // A new subtransaction is started (PostgreSQL semantics).
    EXPECT_EQ(GetCurrentTransactionNestingLevel(), 2);
    EndTransactionBlock();
}

TEST_F(XactTest, NestedSavepoints) {
    BeginTransactionBlock();
    BeginSavepoint("outer");
    BeginSavepoint("inner");
    EXPECT_EQ(GetCurrentTransactionNestingLevel(), 3);
    ReleaseSavepoint("inner");
    EXPECT_EQ(GetCurrentTransactionNestingLevel(), 2);
    ReleaseSavepoint("outer");
    EXPECT_EQ(GetCurrentTransactionNestingLevel(), 1);
    EndTransactionBlock();
}

TEST_F(XactTest, TransactionBlockStateAsString) {
    EXPECT_STREQ(TransactionBlockStateAsString(), "default");

    StartTransactionCommand();
    EXPECT_STREQ(TransactionBlockStateAsString(), "started");
    CommitTransactionCommand();

    BeginTransactionBlock();
    // After BeginTransactionBlock, state should be "begin" or "in progress".
    const char* state = TransactionBlockStateAsString();
    EXPECT_TRUE(std::string(state) == "begin" || std::string(state) == "in progress");
    EndTransactionBlock();
}

// --- IsAbortedTransactionBlock (failed transaction detection for 'Z' status) ---

TEST_F(XactTest, IsAbortedTransactionBlockFalseWhenIdle) {
    EXPECT_FALSE(IsAbortedTransactionBlock());
}

TEST_F(XactTest, IsAbortedTransactionBlockFalseInNormalTransactionBlock) {
    BeginTransactionBlock();
    EXPECT_FALSE(IsAbortedTransactionBlock());
    EXPECT_TRUE(IsTransactionBlock());
    EndTransactionBlock();
}

TEST_F(XactTest, IsAbortedTransactionBlockFalseInAutocommit) {
    StartTransactionCommand();
    EXPECT_FALSE(IsAbortedTransactionBlock());
    CommitTransactionCommand();
}

TEST_F(XactTest, IsAbortedTransactionBlockTrueAfterErrorInBlock) {
    BeginTransactionBlock();
    EXPECT_FALSE(IsAbortedTransactionBlock());
    // Simulate a statement error inside the transaction block.
    AbortCurrentTransaction();
    // The block is now in the aborted state — the 'Z' status should be 'E'.
    EXPECT_TRUE(IsAbortedTransactionBlock());
    EXPECT_TRUE(IsTransactionBlock());  // Still inside a (failed) block.
}

TEST_F(XactTest, IsAbortedTransactionBlockFalseAfterAutocommitError) {
    // An error in autocommit mode aborts the single statement but leaves us
    // idle (not in a failed block).
    StartTransactionCommand();
    AbortCurrentTransaction();
    EXPECT_FALSE(IsAbortedTransactionBlock());
    EXPECT_FALSE(IsTransactionBlock());
}

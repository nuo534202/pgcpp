// twophase_test.cpp — Unit tests for P3-2 two-phase commit (2PC).
//
// Covers:
//   - SaveTwoPhaseState / LookupTwoPhaseState / RemoveTwoPhaseState basics.
//   - Duplicate GID rejection.
//   - CommitPreparedTransaction: marks XID committed, removes record.
//   - RollbackPreparedTransaction: marks XID aborted, removes record.
//   - COMMIT/ROLLBACK PREPARED with unknown GID throws ereport(ERROR).
//   - PrepareTransactionBlock: full PREPARE → COMMIT PREPARED flow.
//   - PrepareTransactionBlock: validation errors (no active tx, subxact open,
//     duplicate GID).
//   - Persistence: prepare, clear in-memory, reload from disk, commit.
//   - After PREPARE, the backend can start a new transaction.

#include "transaction/twophase.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "common/error/elog.hpp"
#include "transaction/transam.hpp"
#include "transaction/xact.hpp"

using pgcpp::error::PgException;
using pgcpp::transaction::BeginSavepoint;
using pgcpp::transaction::BeginTransactionBlock;
using pgcpp::transaction::CommitPreparedTransaction;
using pgcpp::transaction::GetCurrentTransactionId;
using pgcpp::transaction::GetCurrentTransactionIdIfAny;
using pgcpp::transaction::InitializeTransactionSystem;
using pgcpp::transaction::IsolationLevel;
using pgcpp::transaction::kInvalidTransactionId;
using pgcpp::transaction::LoadTwoPhaseFiles;
using pgcpp::transaction::LookupTwoPhaseState;
using pgcpp::transaction::NumTwoPhaseStates;
using pgcpp::transaction::PrepareTransactionBlock;
using pgcpp::transaction::RemoveTwoPhaseState;
using pgcpp::transaction::ResetTransactionState;
using pgcpp::transaction::ResetTwoPhaseState;
using pgcpp::transaction::RollbackPreparedTransaction;
using pgcpp::transaction::SaveTwoPhaseState;
using pgcpp::transaction::SetTwoPhaseDirectory;
using pgcpp::transaction::TransactionId;
using pgcpp::transaction::TransactionIdDidAbort;
using pgcpp::transaction::TransactionIdDidCommit;
using pgcpp::transaction::TwoPhaseState;

namespace {

// Helper: create a fresh temp directory. Returns the path.
std::string MakeTempDir(const std::string& name) {
    std::string path = "/tmp/" + name;
    std::filesystem::remove_all(path);
    std::filesystem::create_directory(path);
    return path;
}

// Helper: remove a directory.
void RemoveTempDir(const std::string& path) {
    std::filesystem::remove_all(path);
}

class TwoPhaseTest : public ::testing::Test {
protected:
    void SetUp() override {
        ResetTransactionState();
        ResetTwoPhaseState();
        InitializeTransactionSystem();
    }
    void TearDown() override {
        ResetTwoPhaseState();
        ResetTransactionState();
        InitializeTransactionSystem();
    }
};

}  // namespace

// ===========================================================================
// TwoPhaseState store: basic CRUD
// ===========================================================================

TEST_F(TwoPhaseTest, SaveAndLookup) {
    TwoPhaseState s;
    s.gid = "tx1";
    s.xid = 100;
    s.isolation_level = IsolationLevel::kSerializable;
    s.read_only = true;
    s.deferrable = true;
    SaveTwoPhaseState(s);

    EXPECT_EQ(NumTwoPhaseStates(), 1u);
    const TwoPhaseState* found = LookupTwoPhaseState("tx1");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->gid, "tx1");
    EXPECT_EQ(found->xid, 100u);
    EXPECT_EQ(found->isolation_level, IsolationLevel::kSerializable);
    EXPECT_TRUE(found->read_only);
    EXPECT_TRUE(found->deferrable);
}

TEST_F(TwoPhaseTest, LookupMissingGidReturnsNull) {
    EXPECT_EQ(LookupTwoPhaseState("nonexistent"), nullptr);
}

TEST_F(TwoPhaseTest, RemoveExistingGid) {
    TwoPhaseState s;
    s.gid = "tx1";
    s.xid = 50;
    SaveTwoPhaseState(s);

    EXPECT_TRUE(RemoveTwoPhaseState("tx1"));
    EXPECT_EQ(NumTwoPhaseStates(), 0u);
    EXPECT_EQ(LookupTwoPhaseState("tx1"), nullptr);
}

TEST_F(TwoPhaseTest, RemoveMissingGidReturnsFalse) {
    EXPECT_FALSE(RemoveTwoPhaseState("nonexistent"));
}

TEST_F(TwoPhaseTest, DuplicateGidThrows) {
    TwoPhaseState s1;
    s1.gid = "dup";
    s1.xid = 10;
    SaveTwoPhaseState(s1);

    TwoPhaseState s2;
    s2.gid = "dup";
    s2.xid = 20;
    EXPECT_THROW(SaveTwoPhaseState(s2), PgException);
    EXPECT_EQ(NumTwoPhaseStates(), 1u);
    // Original entry is unchanged.
    const TwoPhaseState* found = LookupTwoPhaseState("dup");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->xid, 10u);
}

TEST_F(TwoPhaseTest, MultiplePreparedTransactions) {
    TwoPhaseState s1;
    s1.gid = "tx_a";
    s1.xid = 10;
    SaveTwoPhaseState(s1);

    TwoPhaseState s2;
    s2.gid = "tx_b";
    s2.xid = 20;
    SaveTwoPhaseState(s2);

    TwoPhaseState s3;
    s3.gid = "tx_c";
    s3.xid = 30;
    SaveTwoPhaseState(s3);

    EXPECT_EQ(NumTwoPhaseStates(), 3u);
    EXPECT_NE(LookupTwoPhaseState("tx_a"), nullptr);
    EXPECT_NE(LookupTwoPhaseState("tx_b"), nullptr);
    EXPECT_NE(LookupTwoPhaseState("tx_c"), nullptr);

    RemoveTwoPhaseState("tx_b");
    EXPECT_EQ(NumTwoPhaseStates(), 2u);
    EXPECT_EQ(LookupTwoPhaseState("tx_b"), nullptr);
    EXPECT_NE(LookupTwoPhaseState("tx_a"), nullptr);
    EXPECT_NE(LookupTwoPhaseState("tx_c"), nullptr);
}

// ===========================================================================
// CommitPreparedTransaction / RollbackPreparedTransaction
// ===========================================================================

TEST_F(TwoPhaseTest, CommitPreparedMarksXidCommitted) {
    TransactionId xid = 100;
    TwoPhaseState s;
    s.gid = "commit_me";
    s.xid = xid;
    SaveTwoPhaseState(s);

    EXPECT_TRUE(CommitPreparedTransaction("commit_me"));
    EXPECT_EQ(NumTwoPhaseStates(), 0u);
    EXPECT_TRUE(TransactionIdDidCommit(xid));
    EXPECT_FALSE(TransactionIdDidAbort(xid));
}

TEST_F(TwoPhaseTest, RollbackPreparedMarksXidAborted) {
    TransactionId xid = 200;
    TwoPhaseState s;
    s.gid = "rollback_me";
    s.xid = xid;
    SaveTwoPhaseState(s);

    EXPECT_TRUE(RollbackPreparedTransaction("rollback_me"));
    EXPECT_EQ(NumTwoPhaseStates(), 0u);
    EXPECT_TRUE(TransactionIdDidAbort(xid));
    EXPECT_FALSE(TransactionIdDidCommit(xid));
}

TEST_F(TwoPhaseTest, CommitPreparedMissingGidThrows) {
    EXPECT_THROW(CommitPreparedTransaction("nonexistent"), PgException);
    EXPECT_EQ(NumTwoPhaseStates(), 0u);
}

TEST_F(TwoPhaseTest, RollbackPreparedMissingGidThrows) {
    EXPECT_THROW(RollbackPreparedTransaction("nonexistent"), PgException);
    EXPECT_EQ(NumTwoPhaseStates(), 0u);
}

TEST_F(TwoPhaseTest, CommitPreparedDoesNotAffectOtherPrepared) {
    TwoPhaseState s1;
    s1.gid = "tx1";
    s1.xid = 10;
    SaveTwoPhaseState(s1);

    TwoPhaseState s2;
    s2.gid = "tx2";
    s2.xid = 20;
    SaveTwoPhaseState(s2);

    CommitPreparedTransaction("tx1");
    EXPECT_EQ(NumTwoPhaseStates(), 1u);
    EXPECT_EQ(LookupTwoPhaseState("tx1"), nullptr);
    ASSERT_NE(LookupTwoPhaseState("tx2"), nullptr);
    EXPECT_EQ(LookupTwoPhaseState("tx2")->xid, 20u);
    EXPECT_TRUE(TransactionIdDidCommit(10));
    EXPECT_FALSE(TransactionIdDidCommit(20));
}

// ===========================================================================
// PrepareTransactionBlock: full lifecycle via xact API
// ===========================================================================

TEST_F(TwoPhaseTest, PrepareWithoutTransactionBlockThrows) {
    // No BEGIN issued — no active transaction.
    EXPECT_THROW(PrepareTransactionBlock("orphan"), PgException);
    EXPECT_EQ(NumTwoPhaseStates(), 0u);
}

TEST_F(TwoPhaseTest, PrepareAndCommitPrepared) {
    BeginTransactionBlock();
    TransactionId xid = GetCurrentTransactionId();  // force XID assignment

    EXPECT_TRUE(PrepareTransactionBlock("prep1"));
    EXPECT_EQ(NumTwoPhaseStates(), 1u);
    // After PREPARE, the transaction is no longer "current".
    EXPECT_EQ(GetCurrentTransactionIdIfAny(), kInvalidTransactionId);

    // XID is still "in-progress" (neither committed nor aborted).
    EXPECT_FALSE(TransactionIdDidCommit(xid));
    EXPECT_FALSE(TransactionIdDidAbort(xid));

    // COMMIT PREPARED finalizes the transaction.
    EXPECT_TRUE(CommitPreparedTransaction("prep1"));
    EXPECT_EQ(NumTwoPhaseStates(), 0u);
    EXPECT_TRUE(TransactionIdDidCommit(xid));
}

TEST_F(TwoPhaseTest, PrepareAndRollbackPrepared) {
    BeginTransactionBlock();
    TransactionId xid = GetCurrentTransactionId();

    EXPECT_TRUE(PrepareTransactionBlock("prep2"));
    EXPECT_EQ(NumTwoPhaseStates(), 1u);

    EXPECT_TRUE(RollbackPreparedTransaction("prep2"));
    EXPECT_EQ(NumTwoPhaseStates(), 0u);
    EXPECT_TRUE(TransactionIdDidAbort(xid));
}

TEST_F(TwoPhaseTest, PrepareAllowsNewTransactionAfterward) {
    BeginTransactionBlock();
    GetCurrentTransactionId();
    PrepareTransactionBlock("first");

    // Backend can start a new transaction immediately.
    BeginTransactionBlock();
    TransactionId new_xid = GetCurrentTransactionId();
    EXPECT_NE(new_xid, kInvalidTransactionId);
    // The new XID must differ from the prepared one.
    const TwoPhaseState* prep = LookupTwoPhaseState("first");
    ASSERT_NE(prep, nullptr);
    EXPECT_NE(new_xid, prep->xid);

    // Clean up.
    CommitPreparedTransaction("first");
}

TEST_F(TwoPhaseTest, PrepareWithSubtransactionThrows) {
    BeginTransactionBlock();
    BeginSavepoint("sp1");

    EXPECT_THROW(PrepareTransactionBlock("with_sub"), PgException);
    EXPECT_EQ(NumTwoPhaseStates(), 0u);
}

TEST_F(TwoPhaseTest, PrepareDuplicateGidThrows) {
    // First prepare succeeds.
    BeginTransactionBlock();
    GetCurrentTransactionId();
    PrepareTransactionBlock("dup_gid");

    // Second prepare with same GID fails.
    BeginTransactionBlock();
    GetCurrentTransactionId();
    EXPECT_THROW(PrepareTransactionBlock("dup_gid"), PgException);
    EXPECT_EQ(NumTwoPhaseStates(), 1u);

    // Clean up.
    CommitPreparedTransaction("dup_gid");
}

TEST_F(TwoPhaseTest, PreparePersistsIsolationAndFlags) {
    BeginTransactionBlock();
    pgcpp::transaction::SetTransactionIsolationLevel(IsolationLevel::kSerializable);
    pgcpp::transaction::SetTransactionReadOnly(true);
    pgcpp::transaction::SetTransactionDeferrable(true);
    GetCurrentTransactionId();

    PrepareTransactionBlock("flags_tx");

    const TwoPhaseState* s = LookupTwoPhaseState("flags_tx");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->isolation_level, IsolationLevel::kSerializable);
    EXPECT_TRUE(s->read_only);
    EXPECT_TRUE(s->deferrable);

    CommitPreparedTransaction("flags_tx");
}

// ===========================================================================
// Persistence (pg_twophase/)
// ===========================================================================

class TwoPhasePersistenceTest : public ::testing::Test {
protected:
    std::string dir_;

    void SetUp() override {
        dir_ = MakeTempDir("pgcpp_twophase_test");
        ResetTransactionState();
        ResetTwoPhaseState();
        InitializeTransactionSystem();
        SetTwoPhaseDirectory(dir_);
    }
    void TearDown() override {
        ResetTwoPhaseState();
        SetTwoPhaseDirectory("");  // detach from the directory
        RemoveTempDir(dir_);
        ResetTransactionState();
        InitializeTransactionSystem();
    }
};

TEST_F(TwoPhasePersistenceTest, StateFileWrittenToDisk) {
    TwoPhaseState s;
    s.gid = "disk_tx";
    s.xid = 42;
    s.isolation_level = IsolationLevel::kRepeatableRead;
    s.read_only = true;
    s.deferrable = false;
    SaveTwoPhaseState(s);

    // The file should exist (named by hex XID).
    std::string filename = dir_ + "/000000000000002A";
    EXPECT_TRUE(std::filesystem::exists(filename));
}

TEST_F(TwoPhasePersistenceTest, RemoveDeletesStateFile) {
    TwoPhaseState s;
    s.gid = "rm_tx";
    s.xid = 99;
    SaveTwoPhaseState(s);

    std::string filename = dir_ + "/0000000000000063";
    EXPECT_TRUE(std::filesystem::exists(filename));

    RemoveTwoPhaseState("rm_tx");
    EXPECT_FALSE(std::filesystem::exists(filename));
}

TEST_F(TwoPhasePersistenceTest, LoadRecoversFromDisk) {
    // Save a prepared transaction (writes to disk + in-memory).
    TwoPhaseState s;
    s.gid = "recover_me";
    s.xid = 77;
    s.isolation_level = IsolationLevel::kSerializable;
    s.read_only = true;
    s.deferrable = false;
    SaveTwoPhaseState(s);

    // LoadTwoPhaseFiles clears in-memory state first, then reads from disk.
    // This simulates a restart where the in-memory state is empty but the
    // on-disk files persist.
    LoadTwoPhaseFiles();
    EXPECT_EQ(NumTwoPhaseStates(), 1u);

    const TwoPhaseState* loaded = LookupTwoPhaseState("recover_me");
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->xid, 77u);
    EXPECT_EQ(loaded->isolation_level, IsolationLevel::kSerializable);
    EXPECT_TRUE(loaded->read_only);
    EXPECT_FALSE(loaded->deferrable);
}

TEST_F(TwoPhasePersistenceTest, CommitPreparedAfterLoad) {
    TwoPhaseState s;
    s.gid = "load_commit";
    s.xid = 88;
    SaveTwoPhaseState(s);

    // Simulate restart.
    LoadTwoPhaseFiles();

    EXPECT_TRUE(CommitPreparedTransaction("load_commit"));
    EXPECT_EQ(NumTwoPhaseStates(), 0u);
    EXPECT_TRUE(TransactionIdDidCommit(88));

    // File should be removed.
    std::string filename = dir_ + "/0000000000000058";
    EXPECT_FALSE(std::filesystem::exists(filename));
}

TEST_F(TwoPhasePersistenceTest, RollbackPreparedAfterLoad) {
    TwoPhaseState s;
    s.gid = "load_rollback";
    s.xid = 111;
    SaveTwoPhaseState(s);

    LoadTwoPhaseFiles();

    EXPECT_TRUE(RollbackPreparedTransaction("load_rollback"));
    EXPECT_EQ(NumTwoPhaseStates(), 0u);
    EXPECT_TRUE(TransactionIdDidAbort(111));
}

TEST_F(TwoPhasePersistenceTest, LoadEmptyDirectoryIsNoop) {
    // Directory exists but has no files.
    LoadTwoPhaseFiles();
    EXPECT_EQ(NumTwoPhaseStates(), 0u);
}

TEST_F(TwoPhasePersistenceTest, LoadMissingDirectoryIsNoop) {
    // Point to a non-existent directory.
    SetTwoPhaseDirectory("/tmp/pgcpp_twophase_nonexistent_dir");
    LoadTwoPhaseFiles();
    EXPECT_EQ(NumTwoPhaseStates(), 0u);
    // Restore for TearDown.
    SetTwoPhaseDirectory(dir_);
}

TEST_F(TwoPhasePersistenceTest, MultipleStatesPersistAndRecover) {
    TwoPhaseState s1;
    s1.gid = "multi_a";
    s1.xid = 10;
    SaveTwoPhaseState(s1);

    TwoPhaseState s2;
    s2.gid = "multi_b";
    s2.xid = 20;
    SaveTwoPhaseState(s2);

    EXPECT_EQ(NumTwoPhaseStates(), 2u);

    LoadTwoPhaseFiles();
    EXPECT_EQ(NumTwoPhaseStates(), 2u);
    EXPECT_NE(LookupTwoPhaseState("multi_a"), nullptr);
    EXPECT_NE(LookupTwoPhaseState("multi_b"), nullptr);

    // Commit one, verify the other survives.
    CommitPreparedTransaction("multi_a");
    EXPECT_EQ(NumTwoPhaseStates(), 1u);
    EXPECT_NE(LookupTwoPhaseState("multi_b"), nullptr);

    RollbackPreparedTransaction("multi_b");
    EXPECT_EQ(NumTwoPhaseStates(), 0u);
}

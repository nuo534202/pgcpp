// ssi_test.cpp — Unit tests for SSI (Serializable Snapshot Isolation)
// conflict detection (P3-1).
//
// Covers:
//   - IsolationLevel enum: defaults, setters/getters, name parsing.
//   - SSI state: register / release / lookup / reset.
//   - rw-conflict edges: created on write-vs-predicate-lock overlap.
//   - Dangerous structure detection (T1 → T2 → T3 antichain).
//   - OnConflict_CheckForSerializationFailure throws PgException.

#include "transaction/ssi.hpp"

#include <gtest/gtest.h>

#include "common/error/elog.hpp"
#include "storage/ipc/predicate.hpp"
#include "storage/relfilenode.hpp"
#include "transaction/transam.hpp"
#include "transaction/xact.hpp"

using pgcpp::error::PgException;
using pgcpp::storage::PredicateLockRelation;
using pgcpp::storage::PredicateLockTuple;
using pgcpp::storage::RelFileNode;
using pgcpp::transaction::CheckForDangerousStructure;
using pgcpp::transaction::CheckForSerializableConflict;
using pgcpp::transaction::GetSerializableXact;
using pgcpp::transaction::GetTransactionDeferrable;
using pgcpp::transaction::GetTransactionIsolationLevel;
using pgcpp::transaction::GetTransactionReadOnly;
using pgcpp::transaction::IsolationLevel;
using pgcpp::transaction::IsolationLevelName;
using pgcpp::transaction::kInvalidTransactionId;
using pgcpp::transaction::NumSerializableXacts;
using pgcpp::transaction::OnConflict_CheckForSerializationFailure;
using pgcpp::transaction::ParseIsolationLevelName;
using pgcpp::transaction::RegisterSerializableTransaction;
using pgcpp::transaction::ReleaseSerializableTransaction;
using pgcpp::transaction::ResetSSIState;
using pgcpp::transaction::ResetTransactionState;
using pgcpp::transaction::SERIALIZABLEXact;
using pgcpp::transaction::SetTransactionDeferrable;
using pgcpp::transaction::SetTransactionIsolationLevel;
using pgcpp::transaction::SetTransactionReadOnly;
using pgcpp::transaction::TransactionId;

namespace {

// Build a minimal RelFileNode for tests. The values are arbitrary but
// consistent so predicate-lock matching works.
RelFileNode MakeRnode(uint32_t rel) {
    RelFileNode r;
    r.spc_node = 1663;  // pg_default
    r.db_node = 16384;  // first user db
    r.rel_node = rel;
    return r;
}

class SSITest : public ::testing::Test {
protected:
    void SetUp() override {
        ResetTransactionState();
        ResetSSIState();
    }
    void TearDown() override {
        ResetSSIState();
        ResetTransactionState();
    }
};

}  // namespace

// --- IsolationLevel: defaults, set/get, name parsing ---

TEST_F(SSITest, DefaultIsolationIsReadCommitted) {
    // Without an active transaction, the default is READ COMMITTED.
    EXPECT_EQ(GetTransactionIsolationLevel(), IsolationLevel::kReadCommitted);
}

TEST_F(SSITest, ParseIsolationLevelNameAllFour) {
    EXPECT_EQ(ParseIsolationLevelName("read uncommitted"), IsolationLevel::kReadUncommitted);
    EXPECT_EQ(ParseIsolationLevelName("read committed"), IsolationLevel::kReadCommitted);
    EXPECT_EQ(ParseIsolationLevelName("repeatable read"), IsolationLevel::kRepeatableRead);
    EXPECT_EQ(ParseIsolationLevelName("serializable"), IsolationLevel::kSerializable);
}

TEST_F(SSITest, ParseIsolationLevelNameUnknownDefaultsToReadCommitted) {
    EXPECT_EQ(ParseIsolationLevelName("bogus"), IsolationLevel::kReadCommitted);
    EXPECT_EQ(ParseIsolationLevelName(""), IsolationLevel::kReadCommitted);
}

TEST_F(SSITest, IsolationLevelNameRoundtrip) {
    EXPECT_EQ(IsolationLevelName(IsolationLevel::kReadUncommitted), "read uncommitted");
    EXPECT_EQ(IsolationLevelName(IsolationLevel::kReadCommitted), "read committed");
    EXPECT_EQ(IsolationLevelName(IsolationLevel::kRepeatableRead), "repeatable read");
    EXPECT_EQ(IsolationLevelName(IsolationLevel::kSerializable), "serializable");
}

TEST_F(SSITest, SetTransactionIsolationLevelWithoutTransactionIsNoOp) {
    // No active transaction: setter is a silent no-op (does not crash).
    SetTransactionIsolationLevel(IsolationLevel::kSerializable);
    EXPECT_EQ(GetTransactionIsolationLevel(), IsolationLevel::kReadCommitted);
}

TEST_F(SSITest, ReadOnlyAndDeferrableDefaultFalse) {
    EXPECT_FALSE(GetTransactionReadOnly());
    EXPECT_FALSE(GetTransactionDeferrable());
}

TEST_F(SSITest, SetTransactionReadOnlyAndDeferrableWithoutTransactionIsNoOp) {
    SetTransactionReadOnly(true);
    SetTransactionDeferrable(true);
    EXPECT_FALSE(GetTransactionReadOnly());
    EXPECT_FALSE(GetTransactionDeferrable());
}

// --- SSI state registration ---

TEST_F(SSITest, RegisterCreatesState) {
    RegisterSerializableTransaction(100, /*read_only=*/false);
    EXPECT_EQ(NumSerializableXacts(), 1);
    SERIALIZABLEXact* sx = GetSerializableXact(100);
    ASSERT_NE(sx, nullptr);
    EXPECT_EQ(sx->xid, 100u);
    EXPECT_FALSE(sx->finished);
    EXPECT_FALSE(sx->committed);
    EXPECT_FALSE(sx->read_only);
    EXPECT_FALSE(sx->dangerous);
    EXPECT_TRUE(sx->out_conflicts.empty());
    EXPECT_TRUE(sx->in_conflicts.empty());
}

TEST_F(SSITest, RegisterIsIdempotent) {
    RegisterSerializableTransaction(100, false);
    RegisterSerializableTransaction(100, true);  // second call ignored
    EXPECT_EQ(NumSerializableXacts(), 1);
    SERIALIZABLEXact* sx = GetSerializableXact(100);
    ASSERT_NE(sx, nullptr);
    EXPECT_FALSE(sx->read_only);  // first registration wins
}

TEST_F(SSITest, RegisterReadOnlyTransaction) {
    RegisterSerializableTransaction(100, true);
    SERIALIZABLEXact* sx = GetSerializableXact(100);
    ASSERT_NE(sx, nullptr);
    EXPECT_TRUE(sx->read_only);
}

TEST_F(SSITest, GetSerializableXactReturnsNullForUnregistered) {
    EXPECT_EQ(GetSerializableXact(999), nullptr);
}

TEST_F(SSITest, ReleaseMarksFinishedAndCommitted) {
    RegisterSerializableTransaction(100, false);
    ReleaseSerializableTransaction(100, /*committed=*/true);
    SERIALIZABLEXact* sx = GetSerializableXact(100);
    ASSERT_NE(sx, nullptr);
    EXPECT_TRUE(sx->finished);
    EXPECT_TRUE(sx->committed);
}

TEST_F(SSITest, ReleaseMarksFinishedAndAborted) {
    RegisterSerializableTransaction(100, false);
    ReleaseSerializableTransaction(100, /*committed=*/false);
    SERIALIZABLEXact* sx = GetSerializableXact(100);
    ASSERT_NE(sx, nullptr);
    EXPECT_TRUE(sx->finished);
    EXPECT_FALSE(sx->committed);
}

TEST_F(SSITest, ReleaseAbortedReleasesPredicateLocks) {
    RegisterSerializableTransaction(100, false);
    RelFileNode r = MakeRnode(1);
    PredicateLockTuple(r, 0, 1, 100);
    EXPECT_EQ(pgcpp::storage::NumPredicateLocks(), 1);
    ReleaseSerializableTransaction(100, /*committed=*/false);
    EXPECT_EQ(pgcpp::storage::NumPredicateLocks(), 0);
}

TEST_F(SSITest, ReleaseCommittedKeepsPredicateLocks) {
    // PostgreSQL keeps a committed transaction's predicate locks around
    // until no active transaction could conflict with them. pgcpp keeps
    // them until ResetSSIState for correctness.
    RegisterSerializableTransaction(100, false);
    RelFileNode r = MakeRnode(1);
    PredicateLockTuple(r, 0, 1, 100);
    EXPECT_EQ(pgcpp::storage::NumPredicateLocks(), 1);
    ReleaseSerializableTransaction(100, /*committed=*/true);
    EXPECT_EQ(pgcpp::storage::NumPredicateLocks(), 1);
}

TEST_F(SSITest, ResetSSIStateClearsEverything) {
    RegisterSerializableTransaction(100, false);
    RegisterSerializableTransaction(101, false);
    RelFileNode r = MakeRnode(1);
    PredicateLockTuple(r, 0, 1, 100);
    EXPECT_EQ(NumSerializableXacts(), 2);
    EXPECT_EQ(pgcpp::storage::NumPredicateLocks(), 1);
    ResetSSIState();
    EXPECT_EQ(NumSerializableXacts(), 0);
    EXPECT_EQ(pgcpp::storage::NumPredicateLocks(), 0);
    EXPECT_EQ(GetSerializableXact(100), nullptr);
}

// --- rw-conflict detection ---

TEST_F(SSITest, WriteWithNoReadersCreatesNoConflict) {
    RegisterSerializableTransaction(100, false);
    RegisterSerializableTransaction(101, false);
    RelFileNode r = MakeRnode(1);
    // 101 writes to a tuple nobody has read.
    CheckForSerializableConflict(101, r, 0, 1);
    SERIALIZABLEXact* w = GetSerializableXact(101);
    ASSERT_NE(w, nullptr);
    EXPECT_TRUE(w->in_conflicts.empty());
    EXPECT_TRUE(w->out_conflicts.empty());
}

TEST_F(SSITest, WriteOverTuplePredicateLockCreatesConflict) {
    RegisterSerializableTransaction(100, false);  // reader
    RegisterSerializableTransaction(101, false);  // writer
    RelFileNode r = MakeRnode(1);
    // 100 reads (acquires a predicate lock on) tuple (0,1).
    PredicateLockTuple(r, 0, 1, 100);
    // 101 writes to the same tuple.
    CheckForSerializableConflict(101, r, 0, 1);
    SERIALIZABLEXact* reader = GetSerializableXact(100);
    SERIALIZABLEXact* writer = GetSerializableXact(101);
    ASSERT_NE(reader, nullptr);
    ASSERT_NE(writer, nullptr);
    // Edge recorded: reader (100) → writer (101).
    ASSERT_EQ(reader->out_conflicts.size(), 1u);
    EXPECT_EQ(reader->out_conflicts[0].reader, 100u);
    EXPECT_EQ(reader->out_conflicts[0].writer, 101u);
    ASSERT_EQ(writer->in_conflicts.size(), 1u);
    EXPECT_EQ(writer->in_conflicts[0].reader, 100u);
    EXPECT_EQ(writer->in_conflicts[0].writer, 101u);
    // No dangerous structure with only two transactions.
    EXPECT_FALSE(reader->dangerous);
    EXPECT_FALSE(writer->dangerous);
}

TEST_F(SSITest, WriteOverPagePredicateLockCreatesConflict) {
    RegisterSerializableTransaction(100, false);
    RegisterSerializableTransaction(101, false);
    RelFileNode r = MakeRnode(1);
    // 100 acquires a page-level lock on page 0.
    pgcpp::storage::PredicateLockPage(r, 0, 100);
    // 101 writes to tuple (0,5) — covered by page lock.
    CheckForSerializableConflict(101, r, 0, 5);
    SERIALIZABLEXact* writer = GetSerializableXact(101);
    ASSERT_NE(writer, nullptr);
    EXPECT_EQ(writer->in_conflicts.size(), 1u);
}

TEST_F(SSITest, WriteOverRelationPredicateLockCreatesConflict) {
    RegisterSerializableTransaction(100, false);
    RegisterSerializableTransaction(101, false);
    RelFileNode r = MakeRnode(1);
    // 100 acquires a relation-level lock.
    PredicateLockRelation(r, 100);
    // 101 writes anywhere in the relation — covered.
    CheckForSerializableConflict(101, r, 7, 9);
    SERIALIZABLEXact* writer = GetSerializableXact(101);
    ASSERT_NE(writer, nullptr);
    EXPECT_EQ(writer->in_conflicts.size(), 1u);
}

TEST_F(SSITest, WriteOnDifferentRelationCreatesNoConflict) {
    RegisterSerializableTransaction(100, false);
    RegisterSerializableTransaction(101, false);
    PredicateLockTuple(MakeRnode(1), 0, 1, 100);
    // 101 writes to a different relation.
    CheckForSerializableConflict(101, MakeRnode(2), 0, 1);
    SERIALIZABLEXact* writer = GetSerializableXact(101);
    ASSERT_NE(writer, nullptr);
    EXPECT_TRUE(writer->in_conflicts.empty());
}

TEST_F(SSITest, WriteOwnPredicateLockDoesNotCreateConflict) {
    RegisterSerializableTransaction(100, false);
    RelFileNode r = MakeRnode(1);
    PredicateLockTuple(r, 0, 1, 100);
    // 100 writes to the same tuple it has a lock on. No self-conflict.
    CheckForSerializableConflict(100, r, 0, 1);
    SERIALIZABLEXact* writer = GetSerializableXact(100);
    ASSERT_NE(writer, nullptr);
    EXPECT_TRUE(writer->in_conflicts.empty());
    EXPECT_TRUE(writer->out_conflicts.empty());
}

TEST_F(SSITest, ReadOnlyWriterSkipsConflictCheck) {
    RegisterSerializableTransaction(100, /*read_only=*/true);
    RegisterSerializableTransaction(101, false);
    RelFileNode r = MakeRnode(1);
    PredicateLockTuple(r, 0, 1, 101);
    // 100 is read-only: writes should not trigger conflict detection.
    CheckForSerializableConflict(100, r, 0, 1);
    SERIALIZABLEXact* reader = GetSerializableXact(100);
    ASSERT_NE(reader, nullptr);
    EXPECT_TRUE(reader->in_conflicts.empty());
}

TEST_F(SSITest, FinishedWriterSkipsConflictCheck) {
    RegisterSerializableTransaction(100, false);
    RegisterSerializableTransaction(101, false);
    RelFileNode r = MakeRnode(1);
    PredicateLockTuple(r, 0, 1, 100);
    ReleaseSerializableTransaction(101, /*committed=*/true);
    // 101 already finished — no new conflicts.
    CheckForSerializableConflict(101, r, 0, 1);
    SERIALIZABLEXact* writer = GetSerializableXact(101);
    ASSERT_NE(writer, nullptr);
    EXPECT_TRUE(writer->in_conflicts.empty());
}

TEST_F(SSITest, DuplicateWriteDoesNotDuplicateConflictEdge) {
    RegisterSerializableTransaction(100, false);
    RegisterSerializableTransaction(101, false);
    RelFileNode r = MakeRnode(1);
    PredicateLockTuple(r, 0, 1, 100);
    CheckForSerializableConflict(101, r, 0, 1);
    CheckForSerializableConflict(101, r, 0, 1);  // same target again
    SERIALIZABLEXact* writer = GetSerializableXact(101);
    ASSERT_NE(writer, nullptr);
    EXPECT_EQ(writer->in_conflicts.size(), 1u);  // de-duplicated
}

// --- Dangerous structure detection ---

TEST_F(SSITest, DangerousStructureT1ToT2ToT3Middle) {
    // Classic SSI dangerous structure:
    //   T1 reads X, commits.
    //   T2 reads Y, then writes X (conflict T1 → T2).
    //   T3 writes Y (conflict T2 → T3).
    //   T2 commits → dangerous structure detected (T1 < T2 < T3 in
    //   commit order, which cannot occur in any serial execution).
    RegisterSerializableTransaction(1, false);
    RegisterSerializableTransaction(2, false);
    RegisterSerializableTransaction(3, false);
    RelFileNode r = MakeRnode(1);
    // T1 reads X (acquire predicate lock on X).
    PredicateLockTuple(r, 0, 1, 1);
    // T1 commits.
    ReleaseSerializableTransaction(1, true);
    // T2 reads Y (acquire predicate lock on Y).
    PredicateLockTuple(r, 0, 2, 2);
    // T2 writes X — conflict T1 → T2.
    CheckForSerializableConflict(2, r, 0, 1);
    // T3 writes Y — conflict T2 → T3.
    CheckForSerializableConflict(3, r, 0, 2);
    // No dangerous structure yet: T2 hasn't committed, so commit order
    // is not yet determined.
    EXPECT_FALSE(GetSerializableXact(2)->dangerous);
    // T2 commits — completes the dangerous structure T1→T2→T3.
    EXPECT_THROW(ReleaseSerializableTransaction(2, true), PgException);
    // T2 was flagged dangerous before the throw.
    SERIALIZABLEXact* t2 = GetSerializableXact(2);
    ASSERT_NE(t2, nullptr);
    EXPECT_TRUE(t2->dangerous) << "T2 (middle of T1->T2->T3 antichain) should be flagged dangerous";
}

TEST_F(SSITest, DangerousStructureThrowsSerializationFailure) {
    RegisterSerializableTransaction(1, false);
    RegisterSerializableTransaction(2, false);
    RegisterSerializableTransaction(3, false);
    RelFileNode r = MakeRnode(1);
    PredicateLockTuple(r, 0, 1, 1);
    ReleaseSerializableTransaction(1, true);
    PredicateLockTuple(r, 0, 2, 2);
    CheckForSerializableConflict(2, r, 0, 1);
    CheckForSerializableConflict(3, r, 0, 2);
    // T2's commit detects the dangerous structure and throws.
    EXPECT_THROW(ReleaseSerializableTransaction(2, true), PgException);
    // Calling OnConflict again still throws (T2 remains dangerous).
    EXPECT_THROW(OnConflict_CheckForSerializationFailure(2), PgException);
}

TEST_F(SSITest, NoDangerousStructureWithTwoTransactions) {
    RegisterSerializableTransaction(1, false);
    RegisterSerializableTransaction(2, false);
    RelFileNode r = MakeRnode(1);
    PredicateLockTuple(r, 0, 1, 1);
    ReleaseSerializableTransaction(1, true);
    // T2 writes X — only one conflict edge, no T3, no dangerous structure.
    EXPECT_NO_THROW(CheckForSerializableConflict(2, r, 0, 1));
    // T2 commits — still no dangerous structure (no T3 edge).
    EXPECT_NO_THROW(ReleaseSerializableTransaction(2, true));
    SERIALIZABLEXact* t2 = GetSerializableXact(2);
    ASSERT_NE(t2, nullptr);
    EXPECT_FALSE(t2->dangerous);
}

TEST_F(SSITest, NoDangerousStructureWhenT1StillActive) {
    // T1 → T2 → T3 only counts as dangerous if T1 finished before T2's
    // commit. If T1 is still active, no dangerous structure.
    RegisterSerializableTransaction(1, false);
    RegisterSerializableTransaction(2, false);
    RegisterSerializableTransaction(3, false);
    RelFileNode r = MakeRnode(1);
    PredicateLockTuple(r, 0, 1, 1);            // T1 reads X (still active)
    PredicateLockTuple(r, 0, 2, 2);            // T2 reads Y
    CheckForSerializableConflict(2, r, 0, 1);  // T2 writes X — T1 → T2
    CheckForSerializableConflict(3, r, 0, 2);  // T3 writes Y — T2 → T3
    // T2 commits — but T1 hasn't finished, so antichain precondition fails.
    EXPECT_NO_THROW(ReleaseSerializableTransaction(2, true));
    SERIALIZABLEXact* t2 = GetSerializableXact(2);
    ASSERT_NE(t2, nullptr);
    EXPECT_FALSE(t2->dangerous);
}

TEST_F(SSITest, CheckForDangerousStructureReturnsTrueForMiddleCase) {
    RegisterSerializableTransaction(1, false);
    RegisterSerializableTransaction(2, false);
    RegisterSerializableTransaction(3, false);
    RelFileNode r = MakeRnode(1);
    PredicateLockTuple(r, 0, 1, 1);
    ReleaseSerializableTransaction(1, true);
    PredicateLockTuple(r, 0, 2, 2);
    // Build edges without triggering commit-time abort.
    CheckForSerializableConflict(2, r, 0, 1);
    CheckForSerializableConflict(3, r, 0, 2);
    // Now manually call CheckForDangerousStructure on T2.
    EXPECT_TRUE(CheckForDangerousStructure(2));
    // T2 (and T1, T3) should be flagged.
    EXPECT_TRUE(GetSerializableXact(1)->dangerous);
    EXPECT_TRUE(GetSerializableXact(2)->dangerous);
    EXPECT_TRUE(GetSerializableXact(3)->dangerous);
}

// snapmgr_test.cpp — Unit tests for the snapshot stack, CatalogSnapshot, and
// TransactionLogFetch (M7 Task 15.4).
//
// Covers:
//   - Active snapshot stack (push/pop/LIFO, copied snapshots, nested use)
//   - CatalogSnapshot (lazy build, caching, invalidation, reset)
//   - TransactionLogFetch (correctness, caching, invalidation on commit/abort,
//     special-XID fast path)

#include <gtest/gtest.h>

#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "transaction/snapshot.hpp"
#include "transaction/transam.hpp"
#include "transaction/xact.hpp"

using pgcpp::transaction::ActiveSnapshotSet;
using pgcpp::transaction::AllocateNextTransactionId;
using pgcpp::transaction::GetActiveSnapshot;
using pgcpp::transaction::GetCatalogSnapshot;
using pgcpp::transaction::GetNonHistoricCatalogSnapshot;
using pgcpp::transaction::GetTransactionSnapshot;
using pgcpp::transaction::InitializeSnapshotManager;
using pgcpp::transaction::InitializeTransactionSystem;
using pgcpp::transaction::InvalidateCatalogSnapshot;
using pgcpp::transaction::kBootstrapTransactionId;
using pgcpp::transaction::kFrozenTransactionId;
using pgcpp::transaction::kInvalidTransactionId;
using pgcpp::transaction::MakeSnapshot;
using pgcpp::transaction::PopActiveSnapshot;
using pgcpp::transaction::PushActiveSnapshot;
using pgcpp::transaction::PushCopiedSnapshot;
using pgcpp::transaction::ResetTransactionSnapshot;
using pgcpp::transaction::ResetTransactionState;
using pgcpp::transaction::Snapshot;
using pgcpp::transaction::SnapshotData;
using pgcpp::transaction::TransactionId;
using pgcpp::transaction::TransactionIdAbort;
using pgcpp::transaction::TransactionIdCommit;
using pgcpp::transaction::TransactionLogFetch;
using pgcpp::transaction::XidStatus;

namespace {

// The snapshot stack and CatalogSnapshot are file-static and persist across
// tests, so each test must clear them in SetUp/TearDown. Snapshots are
// allocated via makePallocNode, so a memory context is required.
class SnapmgrTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = pgcpp::memory::AllocSetContext::Create("snapmgr_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        ResetTransactionState();
        InitializeTransactionSystem();
        InitializeSnapshotManager();
    }

    void TearDown() override {
        // Clear the stack first so it holds no dangling pointers, then free
        // the memory context that owns the palloc'd snapshots.
        ResetTransactionSnapshot();
        InitializeSnapshotManager();
        ResetTransactionState();
        InitializeTransactionSystem();

        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    pgcpp::memory::MemoryContext* context_ = nullptr;
};

}  // namespace

// --- Active snapshot stack ---

TEST_F(SnapmgrTest, ActiveSnapshotSet_FalseInitially) {
    EXPECT_FALSE(ActiveSnapshotSet());
    EXPECT_EQ(GetActiveSnapshot(), nullptr);
}

TEST_F(SnapmgrTest, PushActiveSnapshot_MakesActiveSet) {
    SnapshotData snap = MakeSnapshot(1, 10);
    PushActiveSnapshot(&snap);
    EXPECT_TRUE(ActiveSnapshotSet());
    PopActiveSnapshot();
    EXPECT_FALSE(ActiveSnapshotSet());
}

TEST_F(SnapmgrTest, PushAndGetActiveSnapshot) {
    SnapshotData snap = MakeSnapshot(1, 10);
    PushActiveSnapshot(&snap);
    EXPECT_EQ(GetActiveSnapshot(), &snap);
    PopActiveSnapshot();
}

TEST_F(SnapmgrTest, PushMultiple_LIFOOrder) {
    SnapshotData s1 = MakeSnapshot(1, 10);
    SnapshotData s2 = MakeSnapshot(2, 20);
    SnapshotData s3 = MakeSnapshot(3, 30);

    PushActiveSnapshot(&s1);
    PushActiveSnapshot(&s2);
    PushActiveSnapshot(&s3);
    EXPECT_EQ(GetActiveSnapshot(), &s3);

    PopActiveSnapshot();
    EXPECT_EQ(GetActiveSnapshot(), &s2);

    PopActiveSnapshot();
    EXPECT_EQ(GetActiveSnapshot(), &s1);

    PopActiveSnapshot();
    EXPECT_FALSE(ActiveSnapshotSet());
}

TEST_F(SnapmgrTest, PopFromEmptyStack) {
    // Popping an empty stack must be a no-op (no crash).
    EXPECT_NO_FATAL_FAILURE(PopActiveSnapshot());
    EXPECT_FALSE(ActiveSnapshotSet());
    EXPECT_EQ(GetActiveSnapshot(), nullptr);
}

TEST_F(SnapmgrTest, PushCopiedSnapshot_MakesIndependentCopy) {
    SnapshotData orig = MakeSnapshot(10, 20);
    PushCopiedSnapshot(&orig);

    Snapshot active = GetActiveSnapshot();
    ASSERT_NE(active, nullptr);
    EXPECT_EQ(active->xmin, 10u);

    // Modifying the original must not affect the pushed copy.
    orig.xmin = 999;
    EXPECT_EQ(active->xmin, 10u);

    PopActiveSnapshot();
}

TEST_F(SnapmgrTest, GetTransactionSnapshot_PushesToStack) {
    EXPECT_FALSE(ActiveSnapshotSet());
    Snapshot s = GetTransactionSnapshot();
    EXPECT_NE(s, nullptr);
    EXPECT_TRUE(ActiveSnapshotSet());
    EXPECT_EQ(GetActiveSnapshot(), s);

    // A second call reuses the stack top (same pointer).
    EXPECT_EQ(GetTransactionSnapshot(), s);
}

TEST_F(SnapmgrTest, ResetTransactionSnapshot_ClearsStack) {
    SnapshotData s1 = MakeSnapshot(1, 10);
    SnapshotData s2 = MakeSnapshot(2, 20);
    PushActiveSnapshot(&s1);
    PushActiveSnapshot(&s2);
    EXPECT_TRUE(ActiveSnapshotSet());

    ResetTransactionSnapshot();
    EXPECT_FALSE(ActiveSnapshotSet());
    EXPECT_EQ(GetActiveSnapshot(), nullptr);
}

TEST_F(SnapmgrTest, NestedPushPop) {
    // Simulate a trigger: push a base snapshot, push a trigger snapshot,
    // execute, then pop to restore the base.
    SnapshotData base = MakeSnapshot(1, 10);
    PushActiveSnapshot(&base);
    EXPECT_EQ(GetActiveSnapshot(), &base);

    SnapshotData trigger = MakeSnapshot(5, 15);
    PushActiveSnapshot(&trigger);
    EXPECT_EQ(GetActiveSnapshot(), &trigger);

    PopActiveSnapshot();
    EXPECT_EQ(GetActiveSnapshot(), &base);

    PopActiveSnapshot();
    EXPECT_FALSE(ActiveSnapshotSet());
}

// --- CatalogSnapshot ---

TEST_F(SnapmgrTest, GetCatalogSnapshot_LazilyBuilds) {
    EXPECT_EQ(GetActiveSnapshot(), nullptr);
    Snapshot s = GetCatalogSnapshot();
    EXPECT_NE(s, nullptr);
}

TEST_F(SnapmgrTest, GetCatalogSnapshot_CachesAcrossCalls) {
    Snapshot s1 = GetCatalogSnapshot();
    Snapshot s2 = GetCatalogSnapshot();
    EXPECT_NE(s1, nullptr);
    EXPECT_EQ(s1, s2);
}

TEST_F(SnapmgrTest, InvalidateCatalogSnapshot_ForcesRebuild) {
    Snapshot s1 = GetCatalogSnapshot();
    ASSERT_NE(s1, nullptr);

    InvalidateCatalogSnapshot();

    Snapshot s2 = GetCatalogSnapshot();
    EXPECT_NE(s2, nullptr);
    EXPECT_NE(s1, s2);
}

TEST_F(SnapmgrTest, GetNonHistoricCatalogSnapshot_ReturnsCatalogWhenSet) {
    Snapshot catalog = GetCatalogSnapshot();
    ASSERT_NE(catalog, nullptr);
    EXPECT_EQ(GetNonHistoricCatalogSnapshot(), catalog);
}

TEST_F(SnapmgrTest, GetNonHistoricCatalogSnapshot_FallsBackToActive) {
    // With no CatalogSnapshot and an active snapshot set, the non-historic
    // variant falls back to the active snapshot.
    SnapshotData snap = MakeSnapshot(1, 10);
    PushActiveSnapshot(&snap);
    EXPECT_EQ(GetNonHistoricCatalogSnapshot(), &snap);
    PopActiveSnapshot();
}

TEST_F(SnapmgrTest, ResetTransactionSnapshot_ClearsCatalogSnapshot) {
    Snapshot s1 = GetCatalogSnapshot();
    ASSERT_NE(s1, nullptr);

    ResetTransactionSnapshot();
    EXPECT_FALSE(ActiveSnapshotSet());

    // After reset, GetCatalogSnapshot builds a fresh snapshot.
    Snapshot s2 = GetCatalogSnapshot();
    EXPECT_NE(s2, nullptr);
    EXPECT_NE(s1, s2);
}

// --- TransactionLogFetch ---

TEST_F(SnapmgrTest, TransactionLogFetch_ReturnsCorrectStatus) {
    TransactionId xid = AllocateNextTransactionId();
    EXPECT_EQ(TransactionLogFetch(xid), XidStatus::kInProgress);

    TransactionIdCommit(xid);
    EXPECT_EQ(TransactionLogFetch(xid), XidStatus::kCommitted);

    TransactionId xid2 = AllocateNextTransactionId();
    TransactionIdAbort(xid2);
    EXPECT_EQ(TransactionLogFetch(xid2), XidStatus::kAborted);
}

TEST_F(SnapmgrTest, TransactionLogFetch_CachesRepeatedLookups) {
    TransactionId xid = AllocateNextTransactionId();
    // Repeated lookups of the same XID must return consistent results
    // (the cache hit must not change the observed status).
    XidStatus s1 = TransactionLogFetch(xid);
    XidStatus s2 = TransactionLogFetch(xid);
    XidStatus s3 = TransactionLogFetch(xid);
    EXPECT_EQ(s1, XidStatus::kInProgress);
    EXPECT_EQ(s2, s1);
    EXPECT_EQ(s3, s1);
}

TEST_F(SnapmgrTest, TransactionLogFetch_SpecialXidsReturnCommitted) {
    EXPECT_EQ(TransactionLogFetch(kBootstrapTransactionId), XidStatus::kCommitted);
    EXPECT_EQ(TransactionLogFetch(kFrozenTransactionId), XidStatus::kCommitted);
    EXPECT_EQ(TransactionLogFetch(kInvalidTransactionId), XidStatus::kCommitted);
}

TEST_F(SnapmgrTest, TransactionLogFetch_InvalidateOnCommit) {
    TransactionId xid = AllocateNextTransactionId();
    EXPECT_EQ(TransactionLogFetch(xid), XidStatus::kInProgress);

    TransactionIdCommit(xid);
    // The cache must have been invalidated, so the next lookup re-reads CLOG.
    EXPECT_EQ(TransactionLogFetch(xid), XidStatus::kCommitted);
}

TEST_F(SnapmgrTest, TransactionLogFetch_InvalidateOnAbort) {
    TransactionId xid = AllocateNextTransactionId();
    EXPECT_EQ(TransactionLogFetch(xid), XidStatus::kInProgress);

    TransactionIdAbort(xid);
    EXPECT_EQ(TransactionLogFetch(xid), XidStatus::kAborted);
}

TEST_F(SnapmgrTest, TransactionLogFetch_DifferentXidsLookup) {
    TransactionId xid1 = AllocateNextTransactionId();
    TransactionId xid2 = AllocateNextTransactionId();

    // Alternate lookups evict the single-entry cache each time; the status
    // returned must still match the underlying CLOG.
    EXPECT_EQ(TransactionLogFetch(xid1), XidStatus::kInProgress);
    EXPECT_EQ(TransactionLogFetch(xid2), XidStatus::kInProgress);

    TransactionIdCommit(xid1);
    EXPECT_EQ(TransactionLogFetch(xid1), XidStatus::kCommitted);
    EXPECT_EQ(TransactionLogFetch(xid2), XidStatus::kInProgress);

    TransactionIdAbort(xid2);
    EXPECT_EQ(TransactionLogFetch(xid1), XidStatus::kCommitted);
    EXPECT_EQ(TransactionLogFetch(xid2), XidStatus::kAborted);
}

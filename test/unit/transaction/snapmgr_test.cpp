// snapmgr_test.cpp — Unit tests for the snapshot stack, CatalogSnapshot, and
// TransactionLogFetch (M7 Task 15.4).
//
// Covers:
//   - Active snapshot stack (push/pop/LIFO, copied snapshots, nested use)
//   - CatalogSnapshot (lazy build, caching, invalidation, reset)
//   - TransactionLogFetch (correctness, caching, invalidation on commit/abort,
//     special-XID fast path)

#include <gtest/gtest.h>

#include "mytoydb/common/error/elog.hpp"
#include "mytoydb/common/memory/alloc_set.hpp"
#include "mytoydb/common/memory/memory_context.hpp"
#include "mytoydb/transaction/snapshot.hpp"
#include "mytoydb/transaction/transam.hpp"
#include "mytoydb/transaction/xact.hpp"

using mytoydb::transaction::ActiveSnapshotSet;
using mytoydb::transaction::AllocateNextTransactionId;
using mytoydb::transaction::GetActiveSnapshot;
using mytoydb::transaction::GetCatalogSnapshot;
using mytoydb::transaction::GetNonHistoricCatalogSnapshot;
using mytoydb::transaction::GetTransactionSnapshot;
using mytoydb::transaction::InitializeSnapshotManager;
using mytoydb::transaction::InitializeTransactionSystem;
using mytoydb::transaction::InvalidateCatalogSnapshot;
using mytoydb::transaction::kBootstrapTransactionId;
using mytoydb::transaction::kFrozenTransactionId;
using mytoydb::transaction::kInvalidTransactionId;
using mytoydb::transaction::MakeSnapshot;
using mytoydb::transaction::PopActiveSnapshot;
using mytoydb::transaction::PushActiveSnapshot;
using mytoydb::transaction::PushCopiedSnapshot;
using mytoydb::transaction::ResetTransactionSnapshot;
using mytoydb::transaction::ResetTransactionState;
using mytoydb::transaction::Snapshot;
using mytoydb::transaction::SnapshotData;
using mytoydb::transaction::TransactionId;
using mytoydb::transaction::TransactionIdAbort;
using mytoydb::transaction::TransactionIdCommit;
using mytoydb::transaction::TransactionLogFetch;
using mytoydb::transaction::XidStatus;

namespace {

// The snapshot stack and CatalogSnapshot are file-static and persist across
// tests, so each test must clear them in SetUp/TearDown. Snapshots are
// allocated via makePallocNode, so a memory context is required.
class SnapmgrTest : public ::testing::Test {
protected:
    void SetUp() override {
        mytoydb::error::InitErrorSubsystem();
        context_ = mytoydb::memory::AllocSetContext::Create("snapmgr_test_context");
        mytoydb::memory::SetCurrentMemoryContext(context_);

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

        mytoydb::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    mytoydb::memory::MemoryContext* context_ = nullptr;
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

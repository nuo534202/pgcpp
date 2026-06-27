// indexam_test.cpp — Unit tests for indexam.c + genam.c (Task 15.8.4).
//
// Tests LookupAmRoutine (btree dispatch), index_open/close, index_build,
// index_insert, index_beginscan/getnext_tid/rescan/endscan, index_can_return,
// index_getbitmap, and the ScanKey helpers (ScanKeyInit / ScanKeyEntryInitialize).
//
// The fixture mirrors nbtree_test.cpp: full stack with catalog, storage,
// buffer pool, and relcache.

#include "mytoydb/access/indexam.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "mytoydb/access/genam.hpp"
#include "mytoydb/access/nbtree.hpp"
#include "mytoydb/access/rel.hpp"
#include "mytoydb/catalog/catalog.hpp"
#include "mytoydb/catalog/pg_class.hpp"
#include "mytoydb/catalog/syscache.hpp"
#include "mytoydb/common/containers/node.hpp"
#include "mytoydb/common/error/elog.hpp"
#include "mytoydb/common/memory/alloc_set.hpp"
#include "mytoydb/common/memory/memory_context.hpp"
#include "mytoydb/storage/bufmgr.hpp"
#include "mytoydb/storage/smgr.hpp"
#include "mytoydb/transaction/heap_tuple.hpp"
#include "mytoydb/transaction/transam.hpp"
#include "mytoydb/transaction/xact.hpp"

using mytoydb::access::btbeginscan;
using mytoydb::access::btbuild;
using mytoydb::access::BTKeyKind;
using mytoydb::access::BTScanDesc;
using mytoydb::access::BTScanKeyData;
using mytoydb::access::BTStrategy;
using mytoydb::access::index_beginscan;
using mytoydb::access::index_build;
using mytoydb::access::index_can_return;
using mytoydb::access::index_close;
using mytoydb::access::index_endscan;
using mytoydb::access::index_getbitmap;
using mytoydb::access::index_getnext_tid;
using mytoydb::access::index_insert;
using mytoydb::access::index_open;
using mytoydb::access::index_rescan;
using mytoydb::access::IndexAmRoutine;
using mytoydb::access::InitializeRelcache;
using mytoydb::access::kBTEqualStrategyNumber;
using mytoydb::access::kBTGreaterStrategyNumber;
using mytoydb::access::kBTreeAmOid;
using mytoydb::access::LookupAmRoutine;
using mytoydb::access::Relation;
using mytoydb::access::RelationClose;
using mytoydb::access::RelationCreateStorage;
using mytoydb::access::RelationOpen;
using mytoydb::access::ResetRelcache;
using mytoydb::access::ScanKeyData;
using mytoydb::access::ScanKeyEntryInitialize;
using mytoydb::access::ScanKeyInit;
using mytoydb::catalog::Catalog;
using mytoydb::catalog::FormData_pg_class;
using mytoydb::catalog::GetCatalog;
using mytoydb::catalog::kInvalidOid;
using mytoydb::catalog::Oid;
using mytoydb::catalog::RelKind;
using mytoydb::catalog::RelPersistence;
using mytoydb::catalog::SetCatalog;
using mytoydb::catalog::SetSysCache;
using mytoydb::catalog::SysCache;
using mytoydb::memory::AllocSetContext;
using mytoydb::storage::BlockNumber;
using mytoydb::storage::InitBufferPool;
using mytoydb::storage::SetStorageBaseDir;
using mytoydb::storage::ShutdownBufferPool;
using mytoydb::storage::smgrcloseall;
using mytoydb::transaction::InitializeTransactionSystem;
using mytoydb::transaction::ItemPointerData;
using mytoydb::transaction::ResetTransactionState;
using mytoydb::types::Datum;
using mytoydb::types::Int32GetDatum;

namespace {

using mytoydb::nodes::makePallocNode;

class IndexamTest : public ::testing::Test {
protected:
    void SetUp() override {
        mytoydb::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("indexam_test_context");
        mytoydb::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        syscache_ = new SysCache();
        SetSysCache(syscache_);

        ResetTransactionState();
        InitializeTransactionSystem();

        test_dir_ = "/tmp/mytoydb_indexam_test_" + std::to_string(getpid());
        SetStorageBaseDir(test_dir_);
        RunShell("rm -rf " + test_dir_);

        InitBufferPool(64);
        InitializeRelcache();
    }

    void TearDown() override {
        ResetRelcache();
        ShutdownBufferPool();
        smgrcloseall();
        RunShell("rm -rf " + test_dir_);

        SetSysCache(nullptr);
        SetCatalog(nullptr);
        delete syscache_;
        delete catalog_;

        ResetTransactionState();
        InitializeTransactionSystem();

        mytoydb::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    // Build a pg_class row for a btree index relation. relam = kBTreeAmOid.
    FormData_pg_class* MakeIndexClassRow(const std::string& name, Oid oid) {
        auto* row = makePallocNode<FormData_pg_class>();
        row->oid = oid;
        row->relname = name;
        row->relfilenode = oid;
        row->relkind = RelKind::kIndex;
        row->relpersistence = RelPersistence::kPermanent;
        row->relam = kBTreeAmOid;
        return row;
    }

    Relation CreateTestIndex(Oid relid, const std::string& name) {
        auto* class_row = MakeIndexClassRow(name, relid);
        catalog_->InsertClass(class_row);
        RelationCreateStorage(relid, false);
        return RelationOpen(relid);
    }

    ItemPointerData MakeTid(BlockNumber block, uint16_t offset) {
        ItemPointerData tid;
        tid.ip_blkid = block;
        tid.ip_posid = offset;
        return tid;
    }

    static void RunShell(const std::string& cmd) { std::system(cmd.c_str()); }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
    SysCache* syscache_ = nullptr;
    std::string test_dir_;
};

// --- LookupAmRoutine ---

TEST_F(IndexamTest, LookupAmRoutineBtreeReturnsTable) {
    const IndexAmRoutine* routine = LookupAmRoutine(kBTreeAmOid);
    ASSERT_NE(routine, nullptr);
    EXPECT_EQ(routine->amoid, kBTreeAmOid);
    EXPECT_NE(routine->aminsert, nullptr);
    EXPECT_NE(routine->ambeginscan, nullptr);
    EXPECT_NE(routine->amgettuple, nullptr);
    EXPECT_NE(routine->ambuild, nullptr);
    EXPECT_NE(routine->amcanreturn, nullptr);
    EXPECT_NE(routine->amgetbitmap, nullptr);
}

TEST_F(IndexamTest, LookupAmRoutineUnknownOidReturnsNull) {
    EXPECT_EQ(LookupAmRoutine(kInvalidOid), nullptr);
    EXPECT_EQ(LookupAmRoutine(9999), nullptr);
}

// --- ScanKey helpers ---

TEST_F(IndexamTest, ScanKeyInitFillsFields) {
    ScanKeyData key;
    ScanKeyInit(&key, /*attno=*/1, kBTEqualStrategyNumber, kInvalidOid, Int32GetDatum(42));
    EXPECT_EQ(key.sk_attno, 1);
    EXPECT_EQ(key.sk_strategy, kBTEqualStrategyNumber);
    EXPECT_EQ(key.sk_argument, Int32GetDatum(42));
    EXPECT_EQ(key.sk_key_kind, BTKeyKind::kInt32);
}

TEST_F(IndexamTest, ScanKeyEntryInitializeFillsAllFields) {
    ScanKeyData key;
    ScanKeyEntryInitialize(&key, /*flags=*/0, /*attno=*/2, kBTGreaterStrategyNumber, kInvalidOid,
                           BTKeyKind::kInt32, 4, Int32GetDatum(7));
    EXPECT_EQ(key.sk_attno, 2);
    EXPECT_EQ(key.sk_strategy, kBTGreaterStrategyNumber);
    EXPECT_EQ(key.sk_key_len, 4);
    EXPECT_EQ(key.sk_argument, Int32GetDatum(7));
}

// --- index_open / index_close / index_build ---

TEST_F(IndexamTest, IndexOpenCloseRoundTrip) {
    constexpr Oid kRelid = 17001;
    Relation rel = CreateTestIndex(kRelid, "idx_open_close");
    ASSERT_NE(rel, nullptr);
    index_build(rel, BTKeyKind::kInt32);

    Relation opened = index_open(kRelid);
    ASSERT_NE(opened, nullptr);
    EXPECT_EQ(opened->rd_id, kRelid);
    index_close(opened);

    // index_close on nullptr is a safe no-op.
    index_close(nullptr);
    RelationClose(rel);
}

// --- index_insert + index_beginscan/getnext_tid/endscan ---

TEST_F(IndexamTest, IndexInsertAndScanRoundTrip) {
    constexpr Oid kRelid = 17002;
    Relation rel = CreateTestIndex(kRelid, "idx_insert_scan");
    index_build(rel, BTKeyKind::kInt32);

    for (int i = 0; i < 10; i++) {
        int32_t key = i * 10;
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i + 1));
        ASSERT_TRUE(index_insert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    // Full scan via the generic API.
    BTScanDesc scan = index_beginscan(rel, BTKeyKind::kInt32, nullptr);
    int count = 0;
    int last_key = -1;
    while (index_getnext_tid(scan)) {
        int key = static_cast<int>(scan->curr_tid.ip_posid - 1) * 10;
        EXPECT_GT(key, last_key);
        last_key = key;
        count++;
    }
    EXPECT_EQ(count, 10);
    index_endscan(scan);

    RelationClose(rel);
}

TEST_F(IndexamTest, IndexCanReturnIsTrueForBtree) {
    constexpr Oid kRelid = 17003;
    Relation rel = CreateTestIndex(kRelid, "idx_canreturn");
    index_build(rel, BTKeyKind::kInt32);
    EXPECT_TRUE(index_can_return(rel));
    RelationClose(rel);
}

TEST_F(IndexamTest, IndexGetbitmapCollectsAllTids) {
    constexpr Oid kRelid = 17004;
    Relation rel = CreateTestIndex(kRelid, "idx_getbitmap");
    index_build(rel, BTKeyKind::kInt32);

    for (int i = 0; i < 20; i++) {
        int32_t key = i;
        ItemPointerData tid =
            MakeTid(static_cast<BlockNumber>(i / 5), static_cast<uint16_t>(i % 5 + 1));
        ASSERT_TRUE(index_insert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    std::vector<ItemPointerData> tids;
    BTScanDesc scan = index_beginscan(rel, BTKeyKind::kInt32, nullptr);
    int64_t n = index_getbitmap(scan, &tids);
    EXPECT_EQ(n, 20);
    EXPECT_EQ(static_cast<int>(tids.size()), 20);
    index_endscan(scan);

    RelationClose(rel);
}

TEST_F(IndexamTest, IndexRescanRestartsScan) {
    constexpr Oid kRelid = 17005;
    Relation rel = CreateTestIndex(kRelid, "idx_rescan");
    index_build(rel, BTKeyKind::kInt32);

    for (int i = 0; i < 5; i++) {
        int32_t key = i;
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i + 1));
        ASSERT_TRUE(index_insert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    BTScanDesc scan = index_beginscan(rel, BTKeyKind::kInt32, nullptr);
    int count1 = 0;
    while (index_getnext_tid(scan)) {
        count1++;
    }
    EXPECT_EQ(count1, 5);

    // Rescan and count again.
    index_rescan(scan, nullptr);
    int count2 = 0;
    while (index_getnext_tid(scan)) {
        count2++;
    }
    EXPECT_EQ(count2, 5);
    index_endscan(scan);

    RelationClose(rel);
}

}  // namespace

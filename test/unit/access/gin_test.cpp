// gin_test.cpp — Unit tests for the GIN index access method (P2-5).
//
// Tests ginbuild, gininsert (int32), ginbeginscan + gingettuple + ginendscan
// (equality lookup, full scan), ginrescan, gingetbitmap, and gincanreturn.

#include "access/gin.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "access/indexam.hpp"
#include "access/nbtpage.hpp"
#include "access/rel.hpp"
#include "catalog/catalog.hpp"
#include "catalog/pg_class.hpp"
#include "catalog/syscache.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "storage/bufmgr.hpp"
#include "storage/smgr.hpp"
#include "transaction/heap_tuple.hpp"
#include "transaction/transam.hpp"
#include "transaction/xact.hpp"

using pgcpp::access::BTKeyKind;
using pgcpp::access::BTScanDesc;
using pgcpp::access::BTScanKeyData;
using pgcpp::access::BTStrategy;
using pgcpp::access::ginbeginscan;
using pgcpp::access::ginbuild;
using pgcpp::access::gincanreturn;
using pgcpp::access::ginendscan;
using pgcpp::access::gingetbitmap;
using pgcpp::access::gingettuple;
using pgcpp::access::gininsert;
using pgcpp::access::ginrescan;
using pgcpp::access::InitializeRelcache;
using pgcpp::access::kGinAmOid;
using pgcpp::access::Relation;
using pgcpp::access::RelationClose;
using pgcpp::access::RelationCreateStorage;
using pgcpp::access::RelationGetNumberOfBlocks;
using pgcpp::access::RelationOpen;
using pgcpp::access::ResetRelcache;
using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_class;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::Oid;
using pgcpp::catalog::RelKind;
using pgcpp::catalog::RelPersistence;
using pgcpp::catalog::SetCatalog;
using pgcpp::catalog::SetSysCache;
using pgcpp::catalog::SysCache;
using pgcpp::memory::AllocSetContext;
using pgcpp::storage::BlockNumber;
using pgcpp::storage::InitBufferPool;
using pgcpp::storage::SetStorageBaseDir;
using pgcpp::storage::ShutdownBufferPool;
using pgcpp::storage::smgrcloseall;
using pgcpp::transaction::InitializeTransactionSystem;
using pgcpp::transaction::ItemPointerData;
using pgcpp::transaction::ResetTransactionState;

namespace {

using pgcpp::nodes::makePallocNode;

class GinIndexTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("gin_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        syscache_ = new SysCache();
        SetSysCache(syscache_);

        ResetTransactionState();
        InitializeTransactionSystem();

        test_dir_ = "/tmp/pgcpp_gin_test_" + std::to_string(getpid());
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

        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    FormData_pg_class* MakeIndexClassRow(const std::string& name, Oid oid) {
        auto* row = makePallocNode<FormData_pg_class>();
        row->oid = oid;
        row->relname = name;
        row->relfilenode = oid;
        row->relkind = RelKind::kIndex;
        row->relpersistence = RelPersistence::kPermanent;
        row->relam = kGinAmOid;
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

    static void RunShell(const std::string& cmd) {
        int rc = std::system(cmd.c_str());
        (void)rc;
    }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
    SysCache* syscache_ = nullptr;
    std::string test_dir_;
};

}  // namespace

// --- ginbuild ---

TEST_F(GinIndexTest, BuildCreatesMetaAndEntryPage) {
    constexpr Oid kRelid = 2200;
    Relation rel = CreateTestIndex(kRelid, "gin_idx1");
    ginbuild(rel, BTKeyKind::kInt32);

    // Meta (block 0) + entry tree page (block 1) = 2 blocks.
    EXPECT_EQ(RelationGetNumberOfBlocks(rel), 2u);
    RelationClose(rel);
}

// --- gininsert + gingettuple (int32 equality) ---

TEST_F(GinIndexTest, InsertSingleInt32AndEqualityScan) {
    constexpr Oid kRelid = 2201;
    Relation rel = CreateTestIndex(kRelid, "gin_idx2");
    ginbuild(rel, BTKeyKind::kInt32);

    int32_t key = 42;
    ItemPointerData tid = MakeTid(0, 1);
    ASSERT_TRUE(gininsert(rel, BTKeyKind::kInt32, &key, 4, tid));

    BTScanKeyData scan_key;
    scan_key.kind = BTKeyKind::kInt32;
    scan_key.key = &key;
    scan_key.key_len = 4;
    scan_key.strategy = BTStrategy::kEqual;

    BTScanDesc scan = ginbeginscan(rel, BTKeyKind::kInt32, &scan_key);
    ASSERT_TRUE(gingettuple(scan));
    EXPECT_EQ(scan->curr_tid, tid);
    EXPECT_FALSE(gingettuple(scan));
    ginendscan(scan);
    RelationClose(rel);
}

TEST_F(GinIndexTest, InsertMultipleTidsForKeyAndEqualityScan) {
    constexpr Oid kRelid = 2202;
    Relation rel = CreateTestIndex(kRelid, "gin_idx3");
    ginbuild(rel, BTKeyKind::kInt32);

    // Insert 3 tids for the same key (GIN multi-tid per entry).
    int32_t key = 100;
    ASSERT_TRUE(gininsert(rel, BTKeyKind::kInt32, &key, 4, MakeTid(0, 1)));
    ASSERT_TRUE(gininsert(rel, BTKeyKind::kInt32, &key, 4, MakeTid(0, 2)));
    ASSERT_TRUE(gininsert(rel, BTKeyKind::kInt32, &key, 4, MakeTid(0, 3)));

    BTScanKeyData scan_key;
    scan_key.kind = BTKeyKind::kInt32;
    scan_key.key = &key;
    scan_key.key_len = 4;
    scan_key.strategy = BTStrategy::kEqual;

    BTScanDesc scan = ginbeginscan(rel, BTKeyKind::kInt32, &scan_key);
    int count = 0;
    while (gingettuple(scan))
        count++;
    EXPECT_EQ(count, 3);
    ginendscan(scan);
    RelationClose(rel);
}

TEST_F(GinIndexTest, InsertMultipleKeysAndFullScan) {
    constexpr Oid kRelid = 2203;
    Relation rel = CreateTestIndex(kRelid, "gin_idx4");
    ginbuild(rel, BTKeyKind::kInt32);

    for (int i = 0; i < 10; i++) {
        int32_t key = i;
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i + 1));
        ASSERT_TRUE(gininsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    BTScanDesc scan = ginbeginscan(rel, BTKeyKind::kInt32, nullptr);
    int count = 0;
    while (gingettuple(scan))
        count++;
    EXPECT_EQ(count, 10);
    ginendscan(scan);
    RelationClose(rel);
}

// --- gingetbitmap ---

TEST_F(GinIndexTest, GetbitmapCollectsAllTids) {
    constexpr Oid kRelid = 2204;
    Relation rel = CreateTestIndex(kRelid, "gin_idx5");
    ginbuild(rel, BTKeyKind::kInt32);

    for (int i = 0; i < 15; i++) {
        int32_t key = i;
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i + 1));
        ASSERT_TRUE(gininsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    std::vector<ItemPointerData> tids;
    BTScanDesc scan = ginbeginscan(rel, BTKeyKind::kInt32, nullptr);
    int64_t n = gingetbitmap(scan, &tids);
    EXPECT_EQ(n, 15);
    ginendscan(scan);
    RelationClose(rel);
}

// --- ginrescan ---

TEST_F(GinIndexTest, RescanRestartsScan) {
    constexpr Oid kRelid = 2205;
    Relation rel = CreateTestIndex(kRelid, "gin_idx6");
    ginbuild(rel, BTKeyKind::kInt32);

    for (int i = 0; i < 5; i++) {
        int32_t key = i;
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i + 1));
        ASSERT_TRUE(gininsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    BTScanDesc scan = ginbeginscan(rel, BTKeyKind::kInt32, nullptr);
    int count1 = 0;
    while (gingettuple(scan))
        count1++;
    EXPECT_EQ(count1, 5);

    ginrescan(scan, nullptr);
    int count2 = 0;
    while (gingettuple(scan))
        count2++;
    EXPECT_EQ(count2, 5);
    ginendscan(scan);
    RelationClose(rel);
}

// --- gincanreturn ---

TEST_F(GinIndexTest, CanReturnIsFalse) {
    constexpr Oid kRelid = 2206;
    Relation rel = CreateTestIndex(kRelid, "gin_idx7");
    ginbuild(rel, BTKeyKind::kInt32);
    EXPECT_FALSE(gincanreturn(rel));
    RelationClose(rel);
}

// gist_test.cpp — Unit tests for the GiST index access method (P2-5).
//
// Tests gistbuild, gistinsert (int32, leaf split), gistbeginscan +
// gistgettuple + gistendscan (equality, range, full scan), gistrescan,
// gistgetbitmap, and gistcanreturn.

#include "access/gist.hpp"

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
using pgcpp::access::gistbeginscan;
using pgcpp::access::gistbuild;
using pgcpp::access::gistcanreturn;
using pgcpp::access::gistendscan;
using pgcpp::access::gistgetbitmap;
using pgcpp::access::gistgettuple;
using pgcpp::access::gistinsert;
using pgcpp::access::gistrescan;
using pgcpp::access::InitializeRelcache;
using pgcpp::access::kGistAmOid;
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

class GistIndexTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("gist_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        syscache_ = new SysCache();
        SetSysCache(syscache_);

        ResetTransactionState();
        InitializeTransactionSystem();

        test_dir_ = "/tmp/pgcpp_gist_test_" + std::to_string(getpid());
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
        row->relam = kGistAmOid;
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

// --- gistbuild ---

TEST_F(GistIndexTest, BuildCreatesMetaAndRoot) {
    constexpr Oid kRelid = 2300;
    Relation rel = CreateTestIndex(kRelid, "gist_idx1");
    gistbuild(rel, BTKeyKind::kInt32);

    // Meta (block 0) + root leaf (block 1) = 2 blocks.
    EXPECT_EQ(RelationGetNumberOfBlocks(rel), 2u);
    RelationClose(rel);
}

// --- gistinsert + gistgettuple ---

TEST_F(GistIndexTest, InsertSingleInt32AndEqualityScan) {
    constexpr Oid kRelid = 2301;
    Relation rel = CreateTestIndex(kRelid, "gist_idx2");
    gistbuild(rel, BTKeyKind::kInt32);

    int32_t key = 42;
    ItemPointerData tid = MakeTid(0, 1);
    ASSERT_TRUE(gistinsert(rel, BTKeyKind::kInt32, &key, 4, tid));

    BTScanKeyData scan_key;
    scan_key.kind = BTKeyKind::kInt32;
    scan_key.key = &key;
    scan_key.key_len = 4;
    scan_key.strategy = BTStrategy::kEqual;

    BTScanDesc scan = gistbeginscan(rel, BTKeyKind::kInt32, &scan_key);
    ASSERT_TRUE(gistgettuple(scan));
    EXPECT_EQ(scan->curr_tid, tid);
    EXPECT_FALSE(gistgettuple(scan));
    gistendscan(scan);
    RelationClose(rel);
}

TEST_F(GistIndexTest, InsertMultipleInt32AndFullScan) {
    constexpr Oid kRelid = 2302;
    Relation rel = CreateTestIndex(kRelid, "gist_idx3");
    gistbuild(rel, BTKeyKind::kInt32);

    for (int i = 0; i < 20; i++) {
        int32_t key = i;
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i + 1));
        ASSERT_TRUE(gistinsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    BTScanDesc scan = gistbeginscan(rel, BTKeyKind::kInt32, nullptr);
    int count = 0;
    while (gistgettuple(scan))
        count++;
    EXPECT_EQ(count, 20);
    gistendscan(scan);
    RelationClose(rel);
}

TEST_F(GistIndexTest, RangeScanGreater) {
    constexpr Oid kRelid = 2303;
    Relation rel = CreateTestIndex(kRelid, "gist_idx4");
    gistbuild(rel, BTKeyKind::kInt32);

    for (int i = 0; i < 20; i++) {
        int32_t key = i;
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i + 1));
        ASSERT_TRUE(gistinsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    int32_t target = 10;
    BTScanKeyData scan_key;
    scan_key.kind = BTKeyKind::kInt32;
    scan_key.key = &target;
    scan_key.key_len = 4;
    scan_key.strategy = BTStrategy::kGreater;

    BTScanDesc scan = gistbeginscan(rel, BTKeyKind::kInt32, &scan_key);
    int count = 0;
    while (gistgettuple(scan))
        count++;
    // Keys 11..19 = 9 entries.
    EXPECT_EQ(count, 9);
    gistendscan(scan);
    RelationClose(rel);
}

TEST_F(GistIndexTest, LeafSplitPreservesAllEntries) {
    constexpr Oid kRelid = 2304;
    Relation rel = CreateTestIndex(kRelid, "gist_idx5");
    gistbuild(rel, BTKeyKind::kInt32);

    // Insert enough entries to force a leaf split.
    for (int i = 0; i < 200; i++) {
        int32_t key = i;
        ItemPointerData tid =
            MakeTid(static_cast<BlockNumber>(i / 10), static_cast<uint16_t>(i % 10 + 1));
        ASSERT_TRUE(gistinsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    BTScanDesc scan = gistbeginscan(rel, BTKeyKind::kInt32, nullptr);
    int count = 0;
    while (gistgettuple(scan))
        count++;
    EXPECT_EQ(count, 200);
    gistendscan(scan);
    RelationClose(rel);
}

// --- gistgetbitmap ---

TEST_F(GistIndexTest, GetbitmapCollectsAllTids) {
    constexpr Oid kRelid = 2305;
    Relation rel = CreateTestIndex(kRelid, "gist_idx6");
    gistbuild(rel, BTKeyKind::kInt32);

    for (int i = 0; i < 15; i++) {
        int32_t key = i;
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i + 1));
        ASSERT_TRUE(gistinsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    std::vector<ItemPointerData> tids;
    BTScanDesc scan = gistbeginscan(rel, BTKeyKind::kInt32, nullptr);
    int64_t n = gistgetbitmap(scan, &tids);
    EXPECT_EQ(n, 15);
    gistendscan(scan);
    RelationClose(rel);
}

// --- gistrescan ---

TEST_F(GistIndexTest, RescanRestartsScan) {
    constexpr Oid kRelid = 2306;
    Relation rel = CreateTestIndex(kRelid, "gist_idx7");
    gistbuild(rel, BTKeyKind::kInt32);

    for (int i = 0; i < 5; i++) {
        int32_t key = i;
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i + 1));
        ASSERT_TRUE(gistinsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    BTScanDesc scan = gistbeginscan(rel, BTKeyKind::kInt32, nullptr);
    int count1 = 0;
    while (gistgettuple(scan))
        count1++;
    EXPECT_EQ(count1, 5);

    gistrescan(scan, nullptr);
    int count2 = 0;
    while (gistgettuple(scan))
        count2++;
    EXPECT_EQ(count2, 5);
    gistendscan(scan);
    RelationClose(rel);
}

// --- gistcanreturn ---

TEST_F(GistIndexTest, CanReturnIsFalse) {
    constexpr Oid kRelid = 2307;
    Relation rel = CreateTestIndex(kRelid, "gist_idx8");
    gistbuild(rel, BTKeyKind::kInt32);
    EXPECT_FALSE(gistcanreturn(rel));
    RelationClose(rel);
}

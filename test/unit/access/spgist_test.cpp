// spgist_test.cpp — Unit tests for the SP-GiST index access method (P2-5).
//
// Tests spgistbuild, spgistinsert (int32, leaf split, trie descent),
// spgistbeginscan + spgistgettuple + spgistendscan (equality, range, full
// scan), spgistrescan, spgistgetbitmap, and spgistcanreturn.

#include "access/spgist.hpp"

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
using pgcpp::access::InitializeRelcache;
using pgcpp::access::kSpgistAmOid;
using pgcpp::access::Relation;
using pgcpp::access::RelationClose;
using pgcpp::access::RelationCreateStorage;
using pgcpp::access::RelationGetNumberOfBlocks;
using pgcpp::access::RelationOpen;
using pgcpp::access::ResetRelcache;
using pgcpp::access::spgistbeginscan;
using pgcpp::access::spgistbuild;
using pgcpp::access::spgistcanreturn;
using pgcpp::access::spgistendscan;
using pgcpp::access::spgistgetbitmap;
using pgcpp::access::spgistgettuple;
using pgcpp::access::spgistinsert;
using pgcpp::access::spgistrescan;
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

class SpgistIndexTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("spgist_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        syscache_ = new SysCache();
        SetSysCache(syscache_);

        ResetTransactionState();
        InitializeTransactionSystem();

        test_dir_ = "/tmp/pgcpp_spgist_test_" + std::to_string(getpid());
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
        row->relam = kSpgistAmOid;
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

// --- spgistbuild ---

TEST_F(SpgistIndexTest, BuildCreatesMetaAndRoot) {
    constexpr Oid kRelid = 2500;
    Relation rel = CreateTestIndex(kRelid, "spgist_idx1");
    spgistbuild(rel, BTKeyKind::kInt32);

    // Meta (block 0) + root leaf (block 1) = 2 blocks.
    EXPECT_EQ(RelationGetNumberOfBlocks(rel), 2u);
    RelationClose(rel);
}

// --- spgistinsert + spgistgettuple ---

TEST_F(SpgistIndexTest, InsertSingleInt32AndEqualityScan) {
    constexpr Oid kRelid = 2501;
    Relation rel = CreateTestIndex(kRelid, "spgist_idx2");
    spgistbuild(rel, BTKeyKind::kInt32);

    int32_t key = 42;
    ItemPointerData tid = MakeTid(0, 1);
    ASSERT_TRUE(spgistinsert(rel, BTKeyKind::kInt32, &key, 4, tid));

    BTScanKeyData scan_key;
    scan_key.kind = BTKeyKind::kInt32;
    scan_key.key = &key;
    scan_key.key_len = 4;
    scan_key.strategy = BTStrategy::kEqual;

    BTScanDesc scan = spgistbeginscan(rel, BTKeyKind::kInt32, &scan_key);
    ASSERT_TRUE(spgistgettuple(scan));
    EXPECT_EQ(scan->curr_tid, tid);
    EXPECT_FALSE(spgistgettuple(scan));
    spgistendscan(scan);
    RelationClose(rel);
}

TEST_F(SpgistIndexTest, InsertMultipleInt32AndFullScan) {
    constexpr Oid kRelid = 2502;
    Relation rel = CreateTestIndex(kRelid, "spgist_idx3");
    spgistbuild(rel, BTKeyKind::kInt32);

    for (int i = 0; i < 20; i++) {
        int32_t key = i;
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i + 1));
        ASSERT_TRUE(spgistinsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    BTScanDesc scan = spgistbeginscan(rel, BTKeyKind::kInt32, nullptr);
    int count = 0;
    while (spgistgettuple(scan))
        count++;
    EXPECT_EQ(count, 20);
    spgistendscan(scan);
    RelationClose(rel);
}

TEST_F(SpgistIndexTest, EqualityScanFindsSpecificKey) {
    constexpr Oid kRelid = 2503;
    Relation rel = CreateTestIndex(kRelid, "spgist_idx4");
    spgistbuild(rel, BTKeyKind::kInt32);

    for (int i = 0; i < 20; i++) {
        int32_t key = i * 3;
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i + 1));
        ASSERT_TRUE(spgistinsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    int32_t target = 30;  // i=10
    BTScanKeyData scan_key;
    scan_key.kind = BTKeyKind::kInt32;
    scan_key.key = &target;
    scan_key.key_len = 4;
    scan_key.strategy = BTStrategy::kEqual;

    BTScanDesc scan = spgistbeginscan(rel, BTKeyKind::kInt32, &scan_key);
    ASSERT_TRUE(spgistgettuple(scan));
    EXPECT_EQ(scan->curr_tid.ip_posid, static_cast<uint16_t>(11));
    EXPECT_FALSE(spgistgettuple(scan));
    spgistendscan(scan);
    RelationClose(rel);
}

TEST_F(SpgistIndexTest, LeafSplitPreservesAllEntries) {
    constexpr Oid kRelid = 2504;
    Relation rel = CreateTestIndex(kRelid, "spgist_idx5");
    spgistbuild(rel, BTKeyKind::kInt32);

    // Insert enough entries to force leaf splits.
    for (int i = 0; i < 200; i++) {
        int32_t key = i;
        ItemPointerData tid =
            MakeTid(static_cast<BlockNumber>(i / 10), static_cast<uint16_t>(i % 10 + 1));
        ASSERT_TRUE(spgistinsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    BTScanDesc scan = spgistbeginscan(rel, BTKeyKind::kInt32, nullptr);
    int count = 0;
    while (spgistgettuple(scan))
        count++;
    EXPECT_EQ(count, 200);
    spgistendscan(scan);
    RelationClose(rel);
}

TEST_F(SpgistIndexTest, RangeScanLessEqual) {
    constexpr Oid kRelid = 2505;
    Relation rel = CreateTestIndex(kRelid, "spgist_idx6");
    spgistbuild(rel, BTKeyKind::kInt32);

    for (int i = 0; i < 20; i++) {
        int32_t key = i;
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i + 1));
        ASSERT_TRUE(spgistinsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    int32_t target = 5;
    BTScanKeyData scan_key;
    scan_key.kind = BTKeyKind::kInt32;
    scan_key.key = &target;
    scan_key.key_len = 4;
    scan_key.strategy = BTStrategy::kLessEqual;

    BTScanDesc scan = spgistbeginscan(rel, BTKeyKind::kInt32, &scan_key);
    int count = 0;
    while (spgistgettuple(scan))
        count++;
    // Keys 0..5 = 6 entries.
    EXPECT_EQ(count, 6);
    spgistendscan(scan);
    RelationClose(rel);
}

// --- spgistgetbitmap ---

TEST_F(SpgistIndexTest, GetbitmapCollectsAllTids) {
    constexpr Oid kRelid = 2506;
    Relation rel = CreateTestIndex(kRelid, "spgist_idx7");
    spgistbuild(rel, BTKeyKind::kInt32);

    for (int i = 0; i < 15; i++) {
        int32_t key = i;
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i + 1));
        ASSERT_TRUE(spgistinsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    std::vector<ItemPointerData> tids;
    BTScanDesc scan = spgistbeginscan(rel, BTKeyKind::kInt32, nullptr);
    int64_t n = spgistgetbitmap(scan, &tids);
    EXPECT_EQ(n, 15);
    spgistendscan(scan);
    RelationClose(rel);
}

// --- spgistrescan ---

TEST_F(SpgistIndexTest, RescanRestartsScan) {
    constexpr Oid kRelid = 2507;
    Relation rel = CreateTestIndex(kRelid, "spgist_idx8");
    spgistbuild(rel, BTKeyKind::kInt32);

    for (int i = 0; i < 5; i++) {
        int32_t key = i;
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i + 1));
        ASSERT_TRUE(spgistinsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    BTScanDesc scan = spgistbeginscan(rel, BTKeyKind::kInt32, nullptr);
    int count1 = 0;
    while (spgistgettuple(scan))
        count1++;
    EXPECT_EQ(count1, 5);

    spgistrescan(scan, nullptr);
    int count2 = 0;
    while (spgistgettuple(scan))
        count2++;
    EXPECT_EQ(count2, 5);
    spgistendscan(scan);
    RelationClose(rel);
}

// --- spgistcanreturn ---

TEST_F(SpgistIndexTest, CanReturnIsFalse) {
    constexpr Oid kRelid = 2508;
    Relation rel = CreateTestIndex(kRelid, "spgist_idx9");
    spgistbuild(rel, BTKeyKind::kInt32);
    EXPECT_FALSE(spgistcanreturn(rel));
    RelationClose(rel);
}

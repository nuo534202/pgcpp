// brin_test.cpp — Unit tests for the BRIN index access method (P2-5).
//
// Tests brinbuild, brininsert (int32, range assignment, summary update),
// brinbeginscan + bringettuple + brinendscan (equality, range, full scan,
// range-skipping), brinrescan, bringetbitmap, and brincanreturn.

#include "access/brin.hpp"

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

using pgcpp::access::brinbeginscan;
using pgcpp::access::brinbuild;
using pgcpp::access::brincanreturn;
using pgcpp::access::brinendscan;
using pgcpp::access::bringetbitmap;
using pgcpp::access::bringettuple;
using pgcpp::access::brininsert;
using pgcpp::access::brinrescan;
using pgcpp::access::BTKeyKind;
using pgcpp::access::BTScanDesc;
using pgcpp::access::BTScanKeyData;
using pgcpp::access::BTStrategy;
using pgcpp::access::InitializeRelcache;
using pgcpp::access::kBrinAmOid;
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

class BrinIndexTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("brin_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        syscache_ = new SysCache();
        SetSysCache(syscache_);

        ResetTransactionState();
        InitializeTransactionSystem();

        test_dir_ = "/tmp/pgcpp_brin_test_" + std::to_string(getpid());
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
        row->relam = kBrinAmOid;
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

// --- brinbuild ---

TEST_F(BrinIndexTest, BuildCreatesMetaPage) {
    constexpr Oid kRelid = 2400;
    Relation rel = CreateTestIndex(kRelid, "brin_idx1");
    brinbuild(rel, BTKeyKind::kInt32);

    // Only meta page (block 0); range pages are created lazily on insert.
    EXPECT_EQ(RelationGetNumberOfBlocks(rel), 1u);
    RelationClose(rel);
}

// --- brininsert + bringettuple ---

TEST_F(BrinIndexTest, InsertSingleInt32AndFullScan) {
    constexpr Oid kRelid = 2401;
    Relation rel = CreateTestIndex(kRelid, "brin_idx1");
    brinbuild(rel, BTKeyKind::kInt32);

    int32_t key = 42;
    ItemPointerData tid = MakeTid(0, 1);
    ASSERT_TRUE(brininsert(rel, BTKeyKind::kInt32, &key, 4, tid));

    BTScanDesc scan = brinbeginscan(rel, BTKeyKind::kInt32, nullptr);
    ASSERT_TRUE(bringettuple(scan));
    EXPECT_EQ(scan->curr_tid, tid);
    EXPECT_FALSE(bringettuple(scan));
    brinendscan(scan);
    RelationClose(rel);
}

TEST_F(BrinIndexTest, InsertMultipleInt32AndFullScan) {
    constexpr Oid kRelid = 2402;
    Relation rel = CreateTestIndex(kRelid, "brin_idx2");
    brinbuild(rel, BTKeyKind::kInt32);

    for (int i = 0; i < 20; i++) {
        int32_t key = i;
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i + 1));
        ASSERT_TRUE(brininsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    BTScanDesc scan = brinbeginscan(rel, BTKeyKind::kInt32, nullptr);
    int count = 0;
    while (bringettuple(scan))
        count++;
    EXPECT_EQ(count, 20);
    brinendscan(scan);
    RelationClose(rel);
}

TEST_F(BrinIndexTest, InsertAcrossMultipleRanges) {
    constexpr Oid kRelid = 2403;
    Relation rel = CreateTestIndex(kRelid, "brin_idx3");
    brinbuild(rel, BTKeyKind::kInt32);

    // Insert into different heap blocks (different ranges).
    // pages_per_range = 16, so blocks 0 and 20 are in different ranges.
    int32_t key1 = 10;
    ItemPointerData tid1 = MakeTid(0, 1);
    ASSERT_TRUE(brininsert(rel, BTKeyKind::kInt32, &key1, 4, tid1));

    int32_t key2 = 99;
    ItemPointerData tid2 = MakeTid(20, 1);
    ASSERT_TRUE(brininsert(rel, BTKeyKind::kInt32, &key2, 4, tid2));

    BTScanDesc scan = brinbeginscan(rel, BTKeyKind::kInt32, nullptr);
    int count = 0;
    while (bringettuple(scan))
        count++;
    EXPECT_EQ(count, 2);
    brinendscan(scan);
    RelationClose(rel);
}

TEST_F(BrinIndexTest, EqualityScanReturnsCandidates) {
    constexpr Oid kRelid = 2404;
    Relation rel = CreateTestIndex(kRelid, "brin_idx4");
    brinbuild(rel, BTKeyKind::kInt32);

    // Range 0 (blocks 0..15): keys 1..5
    for (int i = 1; i <= 5; i++) {
        int32_t key = i;
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i));
        ASSERT_TRUE(brininsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }
    // Range 1 (blocks 16..31): keys 100..105
    for (int i = 0; i < 5; i++) {
        int32_t key = 100 + i;
        ItemPointerData tid = MakeTid(16, static_cast<uint16_t>(i + 1));
        ASSERT_TRUE(brininsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    // Equality scan for key=3 — only range 0 (min=1, max=5) can match.
    // Range 1 (min=100, max=104) should be skipped.
    int32_t target = 3;
    BTScanKeyData scan_key;
    scan_key.kind = BTKeyKind::kInt32;
    scan_key.key = &target;
    scan_key.key_len = 4;
    scan_key.strategy = BTStrategy::kEqual;

    BTScanDesc scan = brinbeginscan(rel, BTKeyKind::kInt32, &scan_key);
    int count = 0;
    while (bringettuple(scan))
        count++;
    // BRIN returns all tids from candidate ranges (range 0: 5 tids).
    EXPECT_EQ(count, 5);
    brinendscan(scan);
    RelationClose(rel);
}

TEST_F(BrinIndexTest, RangeScanGreaterSkipsLowRange) {
    constexpr Oid kRelid = 2405;
    Relation rel = CreateTestIndex(kRelid, "brin_idx5");
    brinbuild(rel, BTKeyKind::kInt32);

    // Range 0: keys 1..5 (max=5)
    for (int i = 1; i <= 5; i++) {
        int32_t key = i;
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i));
        ASSERT_TRUE(brininsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }
    // Range 1: keys 100..105 (max=105)
    for (int i = 0; i < 5; i++) {
        int32_t key = 100 + i;
        ItemPointerData tid = MakeTid(16, static_cast<uint16_t>(i + 1));
        ASSERT_TRUE(brininsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    // key > 50: range 0 (max=5) is skipped, range 1 (max=105) is a candidate.
    int32_t target = 50;
    BTScanKeyData scan_key;
    scan_key.kind = BTKeyKind::kInt32;
    scan_key.key = &target;
    scan_key.key_len = 4;
    scan_key.strategy = BTStrategy::kGreater;

    BTScanDesc scan = brinbeginscan(rel, BTKeyKind::kInt32, &scan_key);
    int count = 0;
    while (bringettuple(scan))
        count++;
    // Only range 1's 5 tids are returned.
    EXPECT_EQ(count, 5);
    brinendscan(scan);
    RelationClose(rel);
}

// --- bringetbitmap ---

TEST_F(BrinIndexTest, GetbitmapCollectsAllTids) {
    constexpr Oid kRelid = 2406;
    Relation rel = CreateTestIndex(kRelid, "brin_idx6");
    brinbuild(rel, BTKeyKind::kInt32);

    for (int i = 0; i < 15; i++) {
        int32_t key = i;
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i + 1));
        ASSERT_TRUE(brininsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    std::vector<ItemPointerData> tids;
    BTScanDesc scan = brinbeginscan(rel, BTKeyKind::kInt32, nullptr);
    int64_t n = bringetbitmap(scan, &tids);
    EXPECT_EQ(n, 15);
    brinendscan(scan);
    RelationClose(rel);
}

// --- brinrescan ---

TEST_F(BrinIndexTest, RescanRestartsScan) {
    constexpr Oid kRelid = 2407;
    Relation rel = CreateTestIndex(kRelid, "brin_idx7");
    brinbuild(rel, BTKeyKind::kInt32);

    for (int i = 0; i < 5; i++) {
        int32_t key = i;
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i + 1));
        ASSERT_TRUE(brininsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    BTScanDesc scan = brinbeginscan(rel, BTKeyKind::kInt32, nullptr);
    int count1 = 0;
    while (bringettuple(scan))
        count1++;
    EXPECT_EQ(count1, 5);

    brinrescan(scan, nullptr);
    int count2 = 0;
    while (bringettuple(scan))
        count2++;
    EXPECT_EQ(count2, 5);
    brinendscan(scan);
    RelationClose(rel);
}

// --- brincanreturn ---

TEST_F(BrinIndexTest, CanReturnIsFalse) {
    constexpr Oid kRelid = 2408;
    Relation rel = CreateTestIndex(kRelid, "brin_idx8");
    brinbuild(rel, BTKeyKind::kInt32);
    EXPECT_FALSE(brincanreturn(rel));
    RelationClose(rel);
}

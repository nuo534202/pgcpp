// hash_test.cpp — Unit tests for the Hash index access method (P2-5).
//
// Tests hashcalc, hashbuild, hashinsert (int32/int64/text), hashbeginscan +
// hashgettuple + hashendscan (equality lookup, full scan), hashrescan,
// hashgetbitmap, and hashcanreturn.

#include "access/hash.hpp"

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
using pgcpp::access::hashbeginscan;
using pgcpp::access::hashbuild;
using pgcpp::access::hashcalc;
using pgcpp::access::hashcanreturn;
using pgcpp::access::hashendscan;
using pgcpp::access::hashgetbitmap;
using pgcpp::access::hashgettuple;
using pgcpp::access::hashinsert;
using pgcpp::access::hashrescan;
using pgcpp::access::InitializeRelcache;
using pgcpp::access::kHashAmOid;
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

class HashIndexTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("hash_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        syscache_ = new SysCache();
        SetSysCache(syscache_);

        ResetTransactionState();
        InitializeTransactionSystem();

        test_dir_ = "/tmp/pgcpp_hash_test_" + std::to_string(getpid());
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
        row->relam = kHashAmOid;
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

// --- hashcalc ---

TEST_F(HashIndexTest, HashcalcInt32Deterministic) {
    int32_t key = 42;
    uint32_t b1 = hashcalc(BTKeyKind::kInt32, &key, 4, 16);
    uint32_t b2 = hashcalc(BTKeyKind::kInt32, &key, 4, 16);
    EXPECT_EQ(b1, b2);
    EXPECT_LT(b1, 16u);
}

TEST_F(HashIndexTest, HashcalcInt64Deterministic) {
    int64_t key = 0x123456789ABCDEF0LL;
    uint32_t b = hashcalc(BTKeyKind::kInt64, &key, 8, 16);
    EXPECT_LT(b, 16u);
}

TEST_F(HashIndexTest, HashcalcTextDeterministic) {
    const char* key = "hello";
    uint32_t b1 = hashcalc(BTKeyKind::kText, key, 5, 16);
    uint32_t b2 = hashcalc(BTKeyKind::kText, key, 5, 16);
    EXPECT_EQ(b1, b2);
    EXPECT_LT(b1, 16u);
}

// --- hashbuild ---

TEST_F(HashIndexTest, BuildCreatesMetaAndBucketPages) {
    constexpr Oid kRelid = 2100;
    Relation rel = CreateTestIndex(kRelid, "hash_idx1");
    hashbuild(rel, BTKeyKind::kInt32);

    // Meta (1) + 16 bucket pages = 17 blocks.
    EXPECT_EQ(RelationGetNumberOfBlocks(rel), 17u);
    RelationClose(rel);
}

// --- hashinsert + hashgettuple (int32) ---

TEST_F(HashIndexTest, InsertSingleInt32AndEqualityScan) {
    constexpr Oid kRelid = 2101;
    Relation rel = CreateTestIndex(kRelid, "hash_idx2");
    hashbuild(rel, BTKeyKind::kInt32);

    int32_t key = 42;
    ItemPointerData tid = MakeTid(0, 1);
    ASSERT_TRUE(hashinsert(rel, BTKeyKind::kInt32, &key, 4, tid));

    BTScanKeyData scan_key;
    scan_key.kind = BTKeyKind::kInt32;
    scan_key.key = &key;
    scan_key.key_len = 4;
    scan_key.strategy = BTStrategy::kEqual;

    BTScanDesc scan = hashbeginscan(rel, BTKeyKind::kInt32, &scan_key);
    ASSERT_TRUE(hashgettuple(scan));
    EXPECT_EQ(scan->curr_tid, tid);
    EXPECT_FALSE(hashgettuple(scan));
    hashendscan(scan);
    RelationClose(rel);
}

TEST_F(HashIndexTest, InsertMultipleInt32AndFullScan) {
    constexpr Oid kRelid = 2102;
    Relation rel = CreateTestIndex(kRelid, "hash_idx3");
    hashbuild(rel, BTKeyKind::kInt32);

    for (int i = 0; i < 20; i++) {
        int32_t key = i * 7;
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i + 1));
        ASSERT_TRUE(hashinsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    BTScanDesc scan = hashbeginscan(rel, BTKeyKind::kInt32, nullptr);
    int count = 0;
    while (hashgettuple(scan)) {
        count++;
    }
    EXPECT_EQ(count, 20);
    hashendscan(scan);
    RelationClose(rel);
}

TEST_F(HashIndexTest, EqualityScanFindsOnlyMatchingKey) {
    constexpr Oid kRelid = 2103;
    Relation rel = CreateTestIndex(kRelid, "hash_idx4");
    hashbuild(rel, BTKeyKind::kInt32);

    for (int i = 0; i < 10; i++) {
        int32_t key = i;
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i + 1));
        ASSERT_TRUE(hashinsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    int32_t target = 5;
    BTScanKeyData scan_key;
    scan_key.kind = BTKeyKind::kInt32;
    scan_key.key = &target;
    scan_key.key_len = 4;
    scan_key.strategy = BTStrategy::kEqual;

    BTScanDesc scan = hashbeginscan(rel, BTKeyKind::kInt32, &scan_key);
    ASSERT_TRUE(hashgettuple(scan));
    EXPECT_EQ(scan->curr_tid.ip_posid, static_cast<uint16_t>(6));
    EXPECT_FALSE(hashgettuple(scan));
    hashendscan(scan);
    RelationClose(rel);
}

// --- hashinsert + hashgettuple (int64) ---

TEST_F(HashIndexTest, InsertAndScanInt64) {
    constexpr Oid kRelid = 2104;
    Relation rel = CreateTestIndex(kRelid, "hash_idx5");
    hashbuild(rel, BTKeyKind::kInt64);

    int64_t key = 1000000LL;
    ItemPointerData tid = MakeTid(2, 3);
    ASSERT_TRUE(hashinsert(rel, BTKeyKind::kInt64, &key, 8, tid));

    BTScanKeyData scan_key;
    scan_key.kind = BTKeyKind::kInt64;
    scan_key.key = &key;
    scan_key.key_len = 8;
    scan_key.strategy = BTStrategy::kEqual;

    BTScanDesc scan = hashbeginscan(rel, BTKeyKind::kInt64, &scan_key);
    ASSERT_TRUE(hashgettuple(scan));
    EXPECT_EQ(scan->curr_tid, tid);
    hashendscan(scan);
    RelationClose(rel);
}

// --- hashinsert + hashgettuple (text) ---

TEST_F(HashIndexTest, InsertAndScanText) {
    constexpr Oid kRelid = 2105;
    Relation rel = CreateTestIndex(kRelid, "hash_idx6");
    hashbuild(rel, BTKeyKind::kText);

    const char* key = "world";
    ItemPointerData tid = MakeTid(1, 1);
    ASSERT_TRUE(hashinsert(rel, BTKeyKind::kText, key, 5, tid));

    BTScanKeyData scan_key;
    scan_key.kind = BTKeyKind::kText;
    scan_key.key = key;
    scan_key.key_len = 5;
    scan_key.strategy = BTStrategy::kEqual;

    BTScanDesc scan = hashbeginscan(rel, BTKeyKind::kText, &scan_key);
    ASSERT_TRUE(hashgettuple(scan));
    EXPECT_EQ(scan->curr_tid, tid);
    hashendscan(scan);
    RelationClose(rel);
}

// --- hashgetbitmap ---

TEST_F(HashIndexTest, GetbitmapCollectsAllTids) {
    constexpr Oid kRelid = 2106;
    Relation rel = CreateTestIndex(kRelid, "hash_idx7");
    hashbuild(rel, BTKeyKind::kInt32);

    for (int i = 0; i < 15; i++) {
        int32_t key = i;
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i + 1));
        ASSERT_TRUE(hashinsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    std::vector<ItemPointerData> tids;
    BTScanDesc scan = hashbeginscan(rel, BTKeyKind::kInt32, nullptr);
    int64_t n = hashgetbitmap(scan, &tids);
    EXPECT_EQ(n, 15);
    EXPECT_EQ(static_cast<int>(tids.size()), 15);
    hashendscan(scan);
    RelationClose(rel);
}

// --- hashrescan ---

TEST_F(HashIndexTest, RescanRestartsScan) {
    constexpr Oid kRelid = 2107;
    Relation rel = CreateTestIndex(kRelid, "hash_idx8");
    hashbuild(rel, BTKeyKind::kInt32);

    for (int i = 0; i < 5; i++) {
        int32_t key = i;
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i + 1));
        ASSERT_TRUE(hashinsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    BTScanDesc scan = hashbeginscan(rel, BTKeyKind::kInt32, nullptr);
    int count1 = 0;
    while (hashgettuple(scan))
        count1++;
    EXPECT_EQ(count1, 5);

    hashrescan(scan, nullptr);
    int count2 = 0;
    while (hashgettuple(scan))
        count2++;
    EXPECT_EQ(count2, 5);
    hashendscan(scan);
    RelationClose(rel);
}

// --- hashcanreturn ---

TEST_F(HashIndexTest, CanReturnIsFalse) {
    constexpr Oid kRelid = 2108;
    Relation rel = CreateTestIndex(kRelid, "hash_idx9");
    hashbuild(rel, BTKeyKind::kInt32);
    EXPECT_FALSE(hashcanreturn(rel));
    RelationClose(rel);
}

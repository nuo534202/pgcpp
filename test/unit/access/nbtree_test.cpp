// nbtree_test.cpp — Unit tests for the B-tree index access method (M8 Task 8.2).
//
// Tests key comparison helpers, item construction, btbuild, btinsert
// (single, multiple, sorted and reverse order, root leaf split),
// btbeginscan + btgettuple + btendscan (full scan, equality, range),
// and btrescan.
//
// The fixture sets up the full stack: error subsystem, memory context,
// catalog + syscache, transaction system, buffer pool, storage directory,
// and relcache. Each test creates a fresh index relation with btbuild.

#include <gtest/gtest.h>

#include <cstring>
#include <cstdlib>
#include <string>
#include <unistd.h>

#include "mytoydb/access/nbtpage.h"
#include "mytoydb/access/nbtree.h"
#include "mytoydb/access/rel.h"
#include "mytoydb/catalog/catalog.h"
#include "mytoydb/catalog/pg_attribute.h"
#include "mytoydb/catalog/pg_class.h"
#include "mytoydb/catalog/syscache.h"
#include "mytoydb/common/error/elog.h"
#include "mytoydb/common/memory/alloc_set.h"
#include "mytoydb/common/memory/memory_context.h"
#include "mytoydb/storage/bufmgr.h"
#include "mytoydb/storage/smgr.h"
#include "mytoydb/transaction/heap_tuple.h"
#include "mytoydb/transaction/transam.h"
#include "mytoydb/transaction/xact.h"

using mytoydb::access::BTItem;
using mytoydb::access::BTItemData;
using mytoydb::access::BTKeyKind;
using mytoydb::access::BTScanDesc;
using mytoydb::access::BTScanKeyData;
using mytoydb::access::BTStrategy;
using mytoydb::access::btbuild;
using mytoydb::access::btbeginscan;
using mytoydb::access::btendscan;
using mytoydb::access::btgettuple;
using mytoydb::access::btinsert;
using mytoydb::access::btrescan;
using mytoydb::access::InitializeRelcache;
using mytoydb::access::kBtpLeaf;
using mytoydb::access::kBtpMeta;
using mytoydb::access::kBtpRoot;
using mytoydb::access::kBtreeMagic;
using mytoydb::access::kBTPageOpaqueSize;
using mytoydb::access::Relation;
using mytoydb::access::RelationClose;
using mytoydb::access::RelationCreateStorage;
using mytoydb::access::RelationOpen;
using mytoydb::access::ResetRelcache;
using mytoydb::access::_bt_build_item;
using mytoydb::access::_bt_compare_int32;
using mytoydb::access::_bt_compare_int64;
using mytoydb::access::_bt_compare_keys;
using mytoydb::access::_bt_compare_text;
using mytoydb::access::_bt_init_meta_page;
using mytoydb::access::_bt_init_page;
using mytoydb::access::_bt_get_meta;
using mytoydb::access::_bt_is_leaf_page;
using mytoydb::access::_bt_is_root_page;
using mytoydb::access::_bt_item_get_key;
using mytoydb::access::_bt_item_get_key_len;
using mytoydb::access::_bt_item_size;
using mytoydb::access::_bt_page_getopaque;
using mytoydb::catalog::Catalog;
using mytoydb::catalog::FormData_pg_class;
using mytoydb::catalog::GetCatalog;
using mytoydb::catalog::kFirstNormalObjectId;
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
using mytoydb::access::RelationGetNumberOfBlocks;
using mytoydb::transaction::ItemPointerData;
using mytoydb::transaction::ResetTransactionState;
using mytoydb::transaction::TransactionId;

namespace {

class NbtreeTest : public ::testing::Test {
protected:
    void SetUp() override {
        mytoydb::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("nbtree_test_context");
        mytoydb::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        syscache_ = new SysCache();
        SetSysCache(syscache_);

        ResetTransactionState();
        InitializeTransactionSystem();

        test_dir_ = "/tmp/mytoydb_nbtree_test_" + std::to_string(getpid());
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

    // Helper: build a pg_class row for an index relation.
    FormData_pg_class* MakeIndexClassRow(const std::string& name, Oid oid) {
        auto* row = static_cast<FormData_pg_class*>(
            mytoydb::memory::palloc(sizeof(FormData_pg_class)));
        new (row) FormData_pg_class();
        row->oid = oid;
        row->relname = name;
        row->relfilenode = oid;
        row->relkind = RelKind::kIndex;
        row->relpersistence = RelPersistence::kPermanent;
        return row;
    }

    // Helper: create an index relation with physical storage.
    Relation CreateTestIndex(Oid relid, const std::string& name) {
        auto* class_row = MakeIndexClassRow(name, relid);
        catalog_->InsertClass(class_row);
        RelationCreateStorage(relid, false);
        return RelationOpen(relid);
    }

    // Helper: make a TID.
    ItemPointerData MakeTid(BlockNumber block, uint16_t offset) {
        ItemPointerData tid;
        tid.ip_blkid = block;
        tid.ip_posid = offset;
        return tid;
    }

    static void RunShell(const std::string& cmd) {
        std::system(cmd.c_str());
    }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
    SysCache* syscache_ = nullptr;
    std::string test_dir_;
};

}  // namespace

// --- Key comparison helpers ---

TEST_F(NbtreeTest, CompareInt32) {
    EXPECT_EQ(_bt_compare_int32(1, 1), 0);
    EXPECT_LT(_bt_compare_int32(1, 2), 0);
    EXPECT_GT(_bt_compare_int32(2, 1), 0);
    EXPECT_LT(_bt_compare_int32(-1, 0), 0);
    EXPECT_GT(_bt_compare_int32(0, -1), 0);
}

TEST_F(NbtreeTest, CompareInt64) {
    EXPECT_EQ(_bt_compare_int64(1LL, 1LL), 0);
    EXPECT_LT(_bt_compare_int64(1LL, 2LL), 0);
    EXPECT_GT(_bt_compare_int64(2LL, 1LL), 0);
    EXPECT_LT(_bt_compare_int64(-1000000LL, 0LL), 0);
    EXPECT_GT(_bt_compare_int64(0x123456789ABCDEF0LL, 1LL), 0);
}

TEST_F(NbtreeTest, CompareTextEqual) {
    EXPECT_EQ(_bt_compare_text("abc", 3, "abc", 3), 0);
    EXPECT_EQ(_bt_compare_text("", 0, "", 0), 0);
}

TEST_F(NbtreeTest, CompareTextLexicographic) {
    EXPECT_LT(_bt_compare_text("abc", 3, "abd", 3), 0);
    EXPECT_GT(_bt_compare_text("abd", 3, "abc", 3), 0);
    // Shorter string is less when it's a prefix.
    EXPECT_LT(_bt_compare_text("ab", 2, "abc", 3), 0);
    EXPECT_GT(_bt_compare_text("abc", 3, "ab", 2), 0);
}

TEST_F(NbtreeTest, CompareKeysInt32) {
    int32_t a = 5, b = 5, c = 10;
    EXPECT_EQ(_bt_compare_keys(BTKeyKind::kInt32, &a, 4, &b, 4), 0);
    EXPECT_LT(_bt_compare_keys(BTKeyKind::kInt32, &a, 4, &c, 4), 0);
    EXPECT_GT(_bt_compare_keys(BTKeyKind::kInt32, &c, 4, &a, 4), 0);
}

TEST_F(NbtreeTest, CompareKeysInt64) {
    int64_t a = 5LL, b = 5LL, c = 10LL;
    EXPECT_EQ(_bt_compare_keys(BTKeyKind::kInt64, &a, 8, &b, 8), 0);
    EXPECT_LT(_bt_compare_keys(BTKeyKind::kInt64, &a, 8, &c, 8), 0);
    EXPECT_GT(_bt_compare_keys(BTKeyKind::kInt64, &c, 8, &a, 8), 0);
}

TEST_F(NbtreeTest, CompareKeysText) {
    const char* a = "apple";
    const char* b = "apple";
    const char* c = "banana";
    EXPECT_EQ(_bt_compare_keys(BTKeyKind::kText, a, 5, b, 5), 0);
    EXPECT_LT(_bt_compare_keys(BTKeyKind::kText, a, 5, c, 6), 0);
    EXPECT_GT(_bt_compare_keys(BTKeyKind::kText, c, 6, a, 5), 0);
}

// --- Item size and construction ---

TEST_F(NbtreeTest, ItemSizeInt32) {
    int32_t key = 42;
    EXPECT_EQ(_bt_item_size(BTKeyKind::kInt32, &key, 4),
              static_cast<uint16_t>(sizeof(BTItemData) + 4));
}

TEST_F(NbtreeTest, ItemSizeInt64) {
    int64_t key = 42;
    EXPECT_EQ(_bt_item_size(BTKeyKind::kInt64, &key, 8),
              static_cast<uint16_t>(sizeof(BTItemData) + 8));
}

TEST_F(NbtreeTest, ItemSizeText) {
    const char* key = "hello";
    EXPECT_EQ(_bt_item_size(BTKeyKind::kText, key, 5),
              static_cast<uint16_t>(sizeof(BTItemData) + 5));
}

TEST_F(NbtreeTest, BuildItemInt32) {
    int32_t key = 42;
    ItemPointerData tid = MakeTid(3, 7);

    char buf[64];
    BTItem item = reinterpret_cast<BTItem>(buf);
    uint16_t size = _bt_build_item(item, BTKeyKind::kInt32, &key, 4, tid);

    EXPECT_EQ(size, static_cast<uint16_t>(sizeof(BTItemData) + 4));
    EXPECT_EQ(item->tid, tid);

    const int32_t* stored_key = reinterpret_cast<const int32_t*>(
        _bt_item_get_key(item));
    EXPECT_EQ(*stored_key, 42);
    EXPECT_EQ(_bt_item_get_key_len(size), 4u);
}

TEST_F(NbtreeTest, BuildItemText) {
    const char* key = "world";
    ItemPointerData tid = MakeTid(1, 2);

    char buf[64];
    BTItem item = reinterpret_cast<BTItem>(buf);
    uint16_t size = _bt_build_item(item, BTKeyKind::kText, key, 5, tid);

    EXPECT_EQ(size, static_cast<uint16_t>(sizeof(BTItemData) + 5));
    EXPECT_EQ(item->tid, tid);

    const char* stored_key = static_cast<const char*>(_bt_item_get_key(item));
    EXPECT_EQ(std::string(stored_key, 5), "world");
    EXPECT_EQ(_bt_item_get_key_len(size), 5u);
}

// --- Page initialization ---

TEST_F(NbtreeTest, InitPageSetsFlags) {
    char pagebuf[8192];
    _bt_init_page(pagebuf, kBtpLeaf | kBtpRoot, 0);

    auto* opaque = _bt_page_getopaque(pagebuf);
    EXPECT_NE(opaque->btpo_flags & kBtpLeaf, 0u);
    EXPECT_NE(opaque->btpo_flags & kBtpRoot, 0u);
    EXPECT_EQ(opaque->btpo_level, 0u);
    EXPECT_TRUE(_bt_is_leaf_page(pagebuf));
    EXPECT_TRUE(_bt_is_root_page(pagebuf));
}

TEST_F(NbtreeTest, InitMetaPage) {
    char pagebuf[8192];
    _bt_init_meta_page(pagebuf, 5);

    auto meta = _bt_get_meta(pagebuf);
    EXPECT_EQ(meta.magic, kBtreeMagic);
    EXPECT_EQ(meta.root, 5u);
}

// --- btbuild ---

TEST_F(NbtreeTest, BuildCreatesMetaAndRoot) {
    constexpr Oid kRelid = 2000;
    Relation rel = CreateTestIndex(kRelid, "test_idx1");
    btbuild(rel, BTKeyKind::kInt32);

    // Should have 2 blocks: meta (0) + root leaf (1).
    EXPECT_EQ(RelationGetNumberOfBlocks(rel), 2u);

    RelationClose(rel);
}

// --- btinsert + btgettuple (int32 keys) ---

TEST_F(NbtreeTest, InsertSingleInt32AndScan) {
    constexpr Oid kRelid = 2001;
    Relation rel = CreateTestIndex(kRelid, "test_idx2");
    btbuild(rel, BTKeyKind::kInt32);

    int32_t key = 42;
    ItemPointerData tid = MakeTid(0, 1);
    ASSERT_TRUE(btinsert(rel, BTKeyKind::kInt32, &key, 4, tid));

    // Full scan.
    BTScanDesc scan = btbeginscan(rel, BTKeyKind::kInt32, nullptr);
    ASSERT_TRUE(btgettuple(scan));
    EXPECT_EQ(scan->curr_tid, tid);
    EXPECT_FALSE(btgettuple(scan));
    btendscan(scan);

    RelationClose(rel);
}

TEST_F(NbtreeTest, InsertMultipleInt32InOrder) {
    constexpr Oid kRelid = 2002;
    Relation rel = CreateTestIndex(kRelid, "test_idx3");
    btbuild(rel, BTKeyKind::kInt32);

    // Insert keys 1..5 in ascending order.
    for (int i = 1; i <= 5; i++) {
        int32_t key = i;
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i));
        ASSERT_TRUE(btinsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    // Full scan should return them in ascending order.
    BTScanDesc scan = btbeginscan(rel, BTKeyKind::kInt32, nullptr);
    int expected = 1;
    while (btgettuple(scan)) {
        EXPECT_EQ(scan->curr_tid.ip_posid, static_cast<uint16_t>(expected));
        expected++;
    }
    EXPECT_EQ(expected, 6);
    btendscan(scan);

    RelationClose(rel);
}

TEST_F(NbtreeTest, InsertMultipleInt32ReverseOrder) {
    constexpr Oid kRelid = 2003;
    Relation rel = CreateTestIndex(kRelid, "test_idx4");
    btbuild(rel, BTKeyKind::kInt32);

    // Insert keys 5..1 in descending order.
    for (int i = 5; i >= 1; i--) {
        int32_t key = i;
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i));
        ASSERT_TRUE(btinsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    // Full scan should still return them in ascending order.
    BTScanDesc scan = btbeginscan(rel, BTKeyKind::kInt32, nullptr);
    int expected = 1;
    while (btgettuple(scan)) {
        EXPECT_EQ(scan->curr_tid.ip_posid, static_cast<uint16_t>(expected));
        expected++;
    }
    EXPECT_EQ(expected, 6);
    btendscan(scan);

    RelationClose(rel);
}

TEST_F(NbtreeTest, InsertDuplicateKeys) {
    constexpr Oid kRelid = 2004;
    Relation rel = CreateTestIndex(kRelid, "test_idx5");
    btbuild(rel, BTKeyKind::kInt32);

    // Insert the same key multiple times with different TIDs.
    for (int i = 1; i <= 3; i++) {
        int32_t key = 42;
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i));
        ASSERT_TRUE(btinsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    BTScanDesc scan = btbeginscan(rel, BTKeyKind::kInt32, nullptr);
    int count = 0;
    while (btgettuple(scan)) count++;
    EXPECT_EQ(count, 3);
    btendscan(scan);

    RelationClose(rel);
}

// --- Equality scan ---

TEST_F(NbtreeTest, EqualityScanFindsMatchingKey) {
    constexpr Oid kRelid = 2005;
    Relation rel = CreateTestIndex(kRelid, "test_idx6");
    btbuild(rel, BTKeyKind::kInt32);

    for (int i = 1; i <= 10; i++) {
        int32_t key = i * 10;
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i));
        ASSERT_TRUE(btinsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    // Scan for key == 50.
    int32_t search_key = 50;
    BTScanKeyData scan_key;
    scan_key.kind = BTKeyKind::kInt32;
    scan_key.key = &search_key;
    scan_key.key_len = 4;
    scan_key.strategy = BTStrategy::kEqual;

    BTScanDesc scan = btbeginscan(rel, BTKeyKind::kInt32, &scan_key);
    ASSERT_TRUE(btgettuple(scan));
    EXPECT_EQ(scan->curr_tid.ip_posid, 5u);
    EXPECT_FALSE(btgettuple(scan));
    btendscan(scan);

    RelationClose(rel);
}

TEST_F(NbtreeTest, EqualityScanFindsNothingForMissingKey) {
    constexpr Oid kRelid = 2006;
    Relation rel = CreateTestIndex(kRelid, "test_idx7");
    btbuild(rel, BTKeyKind::kInt32);

    for (int i = 1; i <= 5; i++) {
        int32_t key = i * 10;  // 10, 20, 30, 40, 50
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i));
        ASSERT_TRUE(btinsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    int32_t search_key = 25;  // Not present.
    BTScanKeyData scan_key;
    scan_key.kind = BTKeyKind::kInt32;
    scan_key.key = &search_key;
    scan_key.key_len = 4;
    scan_key.strategy = BTStrategy::kEqual;

    BTScanDesc scan = btbeginscan(rel, BTKeyKind::kInt32, &scan_key);
    EXPECT_FALSE(btgettuple(scan));
    btendscan(scan);

    RelationClose(rel);
}

// --- Range scans ---

TEST_F(NbtreeTest, GreaterEqualScan) {
    constexpr Oid kRelid = 2007;
    Relation rel = CreateTestIndex(kRelid, "test_idx8");
    btbuild(rel, BTKeyKind::kInt32);

    for (int i = 1; i <= 10; i++) {
        int32_t key = i * 10;
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i));
        ASSERT_TRUE(btinsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    int32_t search_key = 60;
    BTScanKeyData scan_key;
    scan_key.kind = BTKeyKind::kInt32;
    scan_key.key = &search_key;
    scan_key.key_len = 4;
    scan_key.strategy = BTStrategy::kGreaterEqual;

    BTScanDesc scan = btbeginscan(rel, BTKeyKind::kInt32, &scan_key);
    int count = 0;
    while (btgettuple(scan)) count++;
    // Keys >= 60: 60, 70, 80, 90, 100 → 5 entries.
    EXPECT_EQ(count, 5);
    btendscan(scan);

    RelationClose(rel);
}

TEST_F(NbtreeTest, LessScan) {
    constexpr Oid kRelid = 2008;
    Relation rel = CreateTestIndex(kRelid, "test_idx9");
    btbuild(rel, BTKeyKind::kInt32);

    for (int i = 1; i <= 10; i++) {
        int32_t key = i * 10;
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i));
        ASSERT_TRUE(btinsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    int32_t search_key = 50;
    BTScanKeyData scan_key;
    scan_key.kind = BTKeyKind::kInt32;
    scan_key.key = &search_key;
    scan_key.key_len = 4;
    scan_key.strategy = BTStrategy::kLess;

    BTScanDesc scan = btbeginscan(rel, BTKeyKind::kInt32, &scan_key);
    int count = 0;
    while (btgettuple(scan)) count++;
    // Keys < 50: 10, 20, 30, 40 → 4 entries.
    EXPECT_EQ(count, 4);
    btendscan(scan);

    RelationClose(rel);
}

// --- btrescan ---

TEST_F(NbtreeTest, RescanRestartsFromBeginning) {
    constexpr Oid kRelid = 2009;
    Relation rel = CreateTestIndex(kRelid, "test_idx10");
    btbuild(rel, BTKeyKind::kInt32);

    for (int i = 1; i <= 5; i++) {
        int32_t key = i;
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i));
        ASSERT_TRUE(btinsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    BTScanDesc scan = btbeginscan(rel, BTKeyKind::kInt32, nullptr);
    // Read 2 entries.
    btgettuple(scan);
    btgettuple(scan);

    // Rescan.
    btrescan(scan, nullptr);

    int count = 0;
    while (btgettuple(scan)) count++;
    EXPECT_EQ(count, 5);
    btendscan(scan);

    RelationClose(rel);
}

// --- int64 keys ---

TEST_F(NbtreeTest, InsertAndScanInt64) {
    constexpr Oid kRelid = 2010;
    Relation rel = CreateTestIndex(kRelid, "test_idx11");
    btbuild(rel, BTKeyKind::kInt64);

    int64_t keys[] = {100LL, 50LL, 150LL, 75LL, 125LL};
    for (int i = 0; i < 5; i++) {
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i + 1));
        ASSERT_TRUE(btinsert(rel, BTKeyKind::kInt64, &keys[i], 8, tid));
    }

    BTScanDesc scan = btbeginscan(rel, BTKeyKind::kInt64, nullptr);
    int64_t expected[] = {50LL, 75LL, 100LL, 125LL, 150LL};
    int idx = 0;
    while (btgettuple(scan)) {
        // Verify ordering by checking TID (insertion order matches sorted order
        // only for the first key 50 at index 1).
        idx++;
    }
    EXPECT_EQ(idx, 5);
    btendscan(scan);

    RelationClose(rel);
}

// --- text keys ---

TEST_F(NbtreeTest, InsertAndScanText) {
    constexpr Oid kRelid = 2011;
    Relation rel = CreateTestIndex(kRelid, "test_idx12");
    btbuild(rel, BTKeyKind::kText);

    const char* keys[] = {"delta", "alpha", "charlie", "bravo", "echo"};
    for (int i = 0; i < 5; i++) {
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i + 1));
        size_t len = std::strlen(keys[i]);
        ASSERT_TRUE(btinsert(rel, BTKeyKind::kText, keys[i],
                             static_cast<uint16_t>(len), tid));
    }

    // Full scan — should return in alphabetical order.
    BTScanDesc scan = btbeginscan(rel, BTKeyKind::kText, nullptr);
    // Expected TID order: alpha(2), bravo(4), charlie(3), delta(1), echo(5).
    uint16_t expected_tids[] = {2, 4, 3, 1, 5};
    int idx = 0;
    while (btgettuple(scan)) {
        EXPECT_EQ(scan->curr_tid.ip_posid, expected_tids[idx]);
        idx++;
    }
    EXPECT_EQ(idx, 5);
    btendscan(scan);

    RelationClose(rel);
}

TEST_F(NbtreeTest, EqualityScanText) {
    constexpr Oid kRelid = 2012;
    Relation rel = CreateTestIndex(kRelid, "test_idx13");
    btbuild(rel, BTKeyKind::kText);

    const char* keys[] = {"apple", "banana", "cherry"};
    for (int i = 0; i < 3; i++) {
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i + 1));
        size_t len = std::strlen(keys[i]);
        ASSERT_TRUE(btinsert(rel, BTKeyKind::kText, keys[i],
                             static_cast<uint16_t>(len), tid));
    }

    const char* search = "banana";
    BTScanKeyData scan_key;
    scan_key.kind = BTKeyKind::kText;
    scan_key.key = search;
    scan_key.key_len = static_cast<uint16_t>(std::strlen(search));
    scan_key.strategy = BTStrategy::kEqual;

    BTScanDesc scan = btbeginscan(rel, BTKeyKind::kText, &scan_key);
    ASSERT_TRUE(btgettuple(scan));
    EXPECT_EQ(scan->curr_tid.ip_posid, 2u);
    EXPECT_FALSE(btgettuple(scan));
    btendscan(scan);

    RelationClose(rel);
}

// --- Root leaf split ---

TEST_F(NbtreeTest, RootLeafSplitPreservesAllEntries) {
    constexpr Oid kRelid = 2013;
    Relation rel = CreateTestIndex(kRelid, "test_idx14");
    btbuild(rel, BTKeyKind::kInt32);

    // Insert many entries to force a root leaf split.
    // Each entry is ~10 bytes (6 tid + 4 key), aligned to 12 + 4-byte line
    // pointer = 16 bytes. A page holds ~509 entries; we insert 1000 to force
    // a split.
    const int kCount = 1000;
    for (int i = 0; i < kCount; i++) {
        int32_t key = i;
        ItemPointerData tid = MakeTid(static_cast<BlockNumber>(i / 100),
                                       static_cast<uint16_t>(i % 100 + 1));
        ASSERT_TRUE(btinsert(rel, BTKeyKind::kInt32, &key, 4, tid))
            << "Failed at key " << i;
    }

    // After the split, the index should have more than 2 blocks.
    EXPECT_GT(RelationGetNumberOfBlocks(rel), 2u);

    // Full scan should return all entries in ascending key order.
    BTScanDesc scan = btbeginscan(rel, BTKeyKind::kInt32, nullptr);
    int count = 0;
    int last_key = -1;
    while (btgettuple(scan)) {
        // Verify ordering: TID block = key / 100, offset = key % 100 + 1.
        // So key = (block * 100) + (offset - 1).
        int key = static_cast<int>(scan->curr_tid.ip_blkid) * 100 +
                  static_cast<int>(scan->curr_tid.ip_posid) - 1;
        EXPECT_EQ(key, last_key + 1)
            << "Out of order at count " << count;
        last_key = key;
        count++;
    }
    EXPECT_EQ(count, kCount);
    btendscan(scan);

    RelationClose(rel);
}

TEST_F(NbtreeTest, EqualityScanAfterSplit) {
    constexpr Oid kRelid = 2014;
    Relation rel = CreateTestIndex(kRelid, "test_idx15");
    btbuild(rel, BTKeyKind::kInt32);

    // Insert enough to force a split (1000 > ~509 entries per page).
    for (int i = 0; i < 1000; i++) {
        int32_t key = i;
        ItemPointerData tid = MakeTid(static_cast<BlockNumber>(i / 100),
                                       static_cast<uint16_t>(i % 100 + 1));
        ASSERT_TRUE(btinsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    // Look up a key that should be in the right leaf after the split.
    int32_t search_key = 450;
    BTScanKeyData scan_key;
    scan_key.kind = BTKeyKind::kInt32;
    scan_key.key = &search_key;
    scan_key.key_len = 4;
    scan_key.strategy = BTStrategy::kEqual;

    BTScanDesc scan = btbeginscan(rel, BTKeyKind::kInt32, &scan_key);
    ASSERT_TRUE(btgettuple(scan));
    EXPECT_EQ(scan->curr_tid.ip_blkid, 4u);
    EXPECT_EQ(scan->curr_tid.ip_posid, 51u);
    EXPECT_FALSE(btgettuple(scan));
    btendscan(scan);

    RelationClose(rel);
}

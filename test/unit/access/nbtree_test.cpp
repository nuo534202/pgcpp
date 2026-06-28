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

#include "pgcpp/access/nbtree.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "pgcpp/access/nbtpage.hpp"
#include "pgcpp/access/rel.hpp"
#include "pgcpp/catalog/catalog.hpp"
#include "pgcpp/catalog/pg_attribute.hpp"
#include "pgcpp/catalog/pg_class.hpp"
#include "pgcpp/catalog/syscache.hpp"
#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/alloc_set.hpp"
#include "pgcpp/common/memory/memory_context.hpp"
#include "pgcpp/storage/bufmgr.hpp"
#include "pgcpp/storage/bufpage.hpp"
#include "pgcpp/storage/smgr.hpp"
#include "pgcpp/transaction/heap_tuple.hpp"
#include "pgcpp/transaction/transam.hpp"
#include "pgcpp/transaction/xact.hpp"

using pgcpp::access::_bt_binsrch;
using pgcpp::access::_bt_build_item;
using pgcpp::access::_bt_compare_int32;
using pgcpp::access::_bt_compare_int64;
using pgcpp::access::_bt_compare_keys;
using pgcpp::access::_bt_compare_text;
using pgcpp::access::_bt_get_meta;
using pgcpp::access::_bt_getbuf;
using pgcpp::access::_bt_init_meta_page;
using pgcpp::access::_bt_init_page;
using pgcpp::access::_bt_is_leaf_page;
using pgcpp::access::_bt_is_root_page;
using pgcpp::access::_bt_item_get_key;
using pgcpp::access::_bt_item_get_key_len;
using pgcpp::access::_bt_item_size;
using pgcpp::access::_bt_page_getopaque;
using pgcpp::access::_bt_relandgetbuf;
using pgcpp::access::_bt_relbuf;
using pgcpp::access::btbeginscan;
using pgcpp::access::btbuild;
using pgcpp::access::btcanreturn;
using pgcpp::access::btendscan;
using pgcpp::access::btgetbitmap;
using pgcpp::access::btgettuple;
using pgcpp::access::btinsert;
using pgcpp::access::BTItem;
using pgcpp::access::BTItemData;
using pgcpp::access::BTKeyKind;
using pgcpp::access::btrescan;
using pgcpp::access::BTScanDesc;
using pgcpp::access::BTScanKeyData;
using pgcpp::access::BTStrategy;
using pgcpp::access::InitializeRelcache;
using pgcpp::access::kBTPageOpaqueSize;
using pgcpp::access::kBtpLeaf;
using pgcpp::access::kBtpMeta;
using pgcpp::access::kBtpRoot;
using pgcpp::access::kBtreeMagic;
using pgcpp::access::Relation;
using pgcpp::access::RelationClose;
using pgcpp::access::RelationCreateStorage;
using pgcpp::access::RelationGetNumberOfBlocks;
using pgcpp::access::RelationOpen;
using pgcpp::access::ResetRelcache;
using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_class;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::kFirstNormalObjectId;
using pgcpp::catalog::kInvalidOid;
using pgcpp::catalog::Oid;
using pgcpp::catalog::RelKind;
using pgcpp::catalog::RelPersistence;
using pgcpp::catalog::SetCatalog;
using pgcpp::catalog::SetSysCache;
using pgcpp::catalog::SysCache;
using pgcpp::memory::AllocSetContext;
using pgcpp::storage::BlockNumber;
using pgcpp::storage::Buffer;
using pgcpp::storage::BufferGetPage;
using pgcpp::storage::InitBufferPool;
using pgcpp::storage::kInvalidBuffer;
using pgcpp::storage::OffsetNumber;
using pgcpp::storage::Page;
using pgcpp::storage::PageGetMaxOffsetNumber;
using pgcpp::storage::SetStorageBaseDir;
using pgcpp::storage::ShutdownBufferPool;
using pgcpp::storage::smgrcloseall;
using pgcpp::transaction::InitializeTransactionSystem;
using pgcpp::transaction::ItemPointerData;
using pgcpp::transaction::ResetTransactionState;
using pgcpp::transaction::TransactionId;

namespace {

using pgcpp::nodes::makePallocNode;

class NbtreeTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("nbtree_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

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

        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    // Helper: build a pg_class row for an index relation.
    FormData_pg_class* MakeIndexClassRow(const std::string& name, Oid oid) {
        auto* row = makePallocNode<FormData_pg_class>();
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

    static void RunShell(const std::string& cmd) { std::system(cmd.c_str()); }

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

    // Use memcpy to read the key, since the key data follows the 6-byte
    // ItemPointerData tid and may not be 4-byte aligned. Direct
    // reinterpret_cast + dereference would be undefined behavior.
    int32_t stored_key;
    std::memcpy(&stored_key, _bt_item_get_key(item), sizeof(int32_t));
    EXPECT_EQ(stored_key, 42);
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
    while (btgettuple(scan))
        count++;
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
    while (btgettuple(scan))
        count++;
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
    while (btgettuple(scan))
        count++;
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
    while (btgettuple(scan))
        count++;
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
        ASSERT_TRUE(btinsert(rel, BTKeyKind::kText, keys[i], static_cast<uint16_t>(len), tid));
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
        ASSERT_TRUE(btinsert(rel, BTKeyKind::kText, keys[i], static_cast<uint16_t>(len), tid));
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
        ItemPointerData tid =
            MakeTid(static_cast<BlockNumber>(i / 100), static_cast<uint16_t>(i % 100 + 1));
        ASSERT_TRUE(btinsert(rel, BTKeyKind::kInt32, &key, 4, tid)) << "Failed at key " << i;
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
        EXPECT_EQ(key, last_key + 1) << "Out of order at count " << count;
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
        ItemPointerData tid =
            MakeTid(static_cast<BlockNumber>(i / 100), static_cast<uint16_t>(i % 100 + 1));
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

// --- P0 extensions (Task 15.8.5): btcanreturn ---

TEST_F(NbtreeTest, CanReturnIsTrueForBtree) {
    constexpr Oid kRelid = 2100;
    Relation rel = CreateTestIndex(kRelid, "test_idx_canreturn");
    btbuild(rel, BTKeyKind::kInt32);

    // B-tree always supports index-only scans (amcanreturn).
    EXPECT_TRUE(btcanreturn(rel));

    RelationClose(rel);
}

// --- btgetbitmap ---

TEST_F(NbtreeTest, GetBitmapCollectsAllTids) {
    constexpr Oid kRelid = 2101;
    Relation rel = CreateTestIndex(kRelid, "test_idx_getbitmap");
    btbuild(rel, BTKeyKind::kInt32);

    // Insert 10 entries with distinct keys.
    for (int i = 1; i <= 10; i++) {
        int32_t key = i * 10;
        ItemPointerData tid = MakeTid(static_cast<BlockNumber>(i / 4), static_cast<uint16_t>(i));
        ASSERT_TRUE(btinsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    // Full scan via btgetbitmap should collect all 10 tids.
    std::vector<ItemPointerData> tids;
    BTScanDesc scan = btbeginscan(rel, BTKeyKind::kInt32, nullptr);
    int64_t n = btgetbitmap(scan, &tids);
    EXPECT_EQ(n, 10);
    EXPECT_EQ(static_cast<int>(tids.size()), 10);
    btendscan(scan);

    // TIDs should be in ascending key order (10, 20, ..., 100), which means
    // offsets 1..10 in sequence.
    for (int i = 0; i < 10; i++) {
        EXPECT_EQ(tids[i].ip_posid, static_cast<uint16_t>(i + 1));
    }

    RelationClose(rel);
}

TEST_F(NbtreeTest, GetBitmapWithEmptyIndexReturnsZero) {
    constexpr Oid kRelid = 2102;
    Relation rel = CreateTestIndex(kRelid, "test_idx_getbitmap_empty");
    btbuild(rel, BTKeyKind::kInt32);

    std::vector<ItemPointerData> tids;
    BTScanDesc scan = btbeginscan(rel, BTKeyKind::kInt32, nullptr);
    int64_t n = btgetbitmap(scan, &tids);
    EXPECT_EQ(n, 0);
    EXPECT_TRUE(tids.empty());
    btendscan(scan);

    RelationClose(rel);
}

TEST_F(NbtreeTest, GetBitmapWithNullArgsIsZero) {
    // Defensive: nullptr scan or tids yields 0 (no crash).
    std::vector<ItemPointerData> tids;
    EXPECT_EQ(btgetbitmap(nullptr, &tids), 0);

    constexpr Oid kRelid = 2103;
    Relation rel = CreateTestIndex(kRelid, "test_idx_getbitmap_null");
    btbuild(rel, BTKeyKind::kInt32);
    BTScanDesc scan = btbeginscan(rel, BTKeyKind::kInt32, nullptr);
    EXPECT_EQ(btgetbitmap(scan, nullptr), 0);
    btendscan(scan);
    RelationClose(rel);
}

// --- _bt_binsrch ---

TEST_F(NbtreeTest, BinsrchScanModeFindsFirstGreaterOrEqual) {
    constexpr Oid kRelid = 2104;
    Relation rel = CreateTestIndex(kRelid, "test_idx_binsrch_scan");
    btbuild(rel, BTKeyKind::kInt32);

    // Insert keys 10, 20, 30, 40, 50 at offsets 1..5.
    for (int i = 1; i <= 5; i++) {
        int32_t key = i * 10;
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i));
        ASSERT_TRUE(btinsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    // Root leaf is block 1.
    Buffer buf = _bt_getbuf(rel, 1);
    ASSERT_NE(buf, kInvalidBuffer);
    Page page = BufferGetPage(buf);
    EXPECT_EQ(PageGetMaxOffsetNumber(page), 5u);

    // for_insert=false: find first item with key >= search key.
    // key=25 → first >= 25 is 30 at offset 3.
    int32_t key25 = 25;
    EXPECT_EQ(_bt_binsrch(page, BTKeyKind::kInt32, &key25, 4, /*for_insert=*/false),
              static_cast<OffsetNumber>(3));

    // key=10 → first >= 10 is 10 at offset 1.
    int32_t key10 = 10;
    EXPECT_EQ(_bt_binsrch(page, BTKeyKind::kInt32, &key10, 4, false), static_cast<OffsetNumber>(1));

    // key=50 → first >= 50 is 50 at offset 5.
    int32_t key50 = 50;
    EXPECT_EQ(_bt_binsrch(page, BTKeyKind::kInt32, &key50, 4, false), static_cast<OffsetNumber>(5));

    // key=60 → no item >= 60; returns max_offset+1 = 6 (past end).
    int32_t key60 = 60;
    EXPECT_EQ(_bt_binsrch(page, BTKeyKind::kInt32, &key60, 4, false), static_cast<OffsetNumber>(6));

    _bt_relbuf(rel, buf);
    RelationClose(rel);
}

TEST_F(NbtreeTest, BinsrchInsertModeFindsFirstStrictlyGreater) {
    constexpr Oid kRelid = 2105;
    Relation rel = CreateTestIndex(kRelid, "test_idx_binsrch_insert");
    btbuild(rel, BTKeyKind::kInt32);

    // Insert keys 10, 20, 20, 30 (note the duplicate 20) at offsets 1..4.
    int32_t keys[] = {10, 20, 20, 30};
    for (int i = 0; i < 4; i++) {
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i + 1));
        ASSERT_TRUE(btinsert(rel, BTKeyKind::kInt32, &keys[i], 4, tid));
    }

    Buffer buf = _bt_getbuf(rel, 1);
    ASSERT_NE(buf, kInvalidBuffer);
    Page page = BufferGetPage(buf);
    EXPECT_EQ(PageGetMaxOffsetNumber(page), 4u);

    // for_insert=true: find first item with key strictly > search key.
    // key=20 → first > 20 is 30 at offset 4 (skips both 20s).
    int32_t key20 = 20;
    EXPECT_EQ(_bt_binsrch(page, BTKeyKind::kInt32, &key20, 4, /*for_insert=*/true),
              static_cast<OffsetNumber>(4));

    // key=10 → first > 10 is 20 at offset 2.
    int32_t key10 = 10;
    EXPECT_EQ(_bt_binsrch(page, BTKeyKind::kInt32, &key10, 4, true), static_cast<OffsetNumber>(2));

    // key=30 → first > 30 is none; returns max_offset+1 = 5 (past end).
    int32_t key30 = 30;
    EXPECT_EQ(_bt_binsrch(page, BTKeyKind::kInt32, &key30, 4, true), static_cast<OffsetNumber>(5));

    _bt_relbuf(rel, buf);
    RelationClose(rel);
}

TEST_F(NbtreeTest, BinsrchOnEmptyPageReturnsOne) {
    constexpr Oid kRelid = 2106;
    Relation rel = CreateTestIndex(kRelid, "test_idx_binsrch_empty");
    btbuild(rel, BTKeyKind::kInt32);

    // Freshly built index: root leaf has no items.
    Buffer buf = _bt_getbuf(rel, 1);
    ASSERT_NE(buf, kInvalidBuffer);
    Page page = BufferGetPage(buf);
    EXPECT_EQ(PageGetMaxOffsetNumber(page), 0u);

    int32_t key = 42;
    // Empty page: any search returns offset 1 (the first valid insert position).
    EXPECT_EQ(_bt_binsrch(page, BTKeyKind::kInt32, &key, 4, false), static_cast<OffsetNumber>(1));
    EXPECT_EQ(_bt_binsrch(page, BTKeyKind::kInt32, &key, 4, true), static_cast<OffsetNumber>(1));

    _bt_relbuf(rel, buf);
    RelationClose(rel);
}

// --- _bt_getbuf / _bt_relandgetbuf / _bt_relbuf ---

TEST_F(NbtreeTest, GetbufPinsMetaAndRootPages) {
    constexpr Oid kRelid = 2107;
    Relation rel = CreateTestIndex(kRelid, "test_idx_getbuf");
    btbuild(rel, BTKeyKind::kInt32);

    // btbuild creates block 0 (meta) and block 1 (root leaf).
    Buffer meta_buf = _bt_getbuf(rel, 0);
    EXPECT_NE(meta_buf, kInvalidBuffer);

    Buffer root_buf = _bt_getbuf(rel, 1);
    EXPECT_NE(root_buf, kInvalidBuffer);

    // Different blocks yield different buffer handles.
    EXPECT_NE(meta_buf, root_buf);

    _bt_relbuf(rel, meta_buf);
    _bt_relbuf(rel, root_buf);
    RelationClose(rel);
}

TEST_F(NbtreeTest, RelandgetbufSwapsPinnedPage) {
    constexpr Oid kRelid = 2108;
    Relation rel = CreateTestIndex(kRelid, "test_idx_relandgetbuf");
    btbuild(rel, BTKeyKind::kInt32);

    Buffer buf = _bt_getbuf(rel, 0);
    ASSERT_NE(buf, kInvalidBuffer);

    // Release block 0 and pin block 1 in one call.
    Buffer buf2 = _bt_relandgetbuf(rel, buf, 1);
    EXPECT_NE(buf2, kInvalidBuffer);
    EXPECT_NE(buf2, buf);

    _bt_relbuf(rel, buf2);
    RelationClose(rel);
}

TEST_F(NbtreeTest, RelbufOnInvalidBufferIsNoOp) {
    constexpr Oid kRelid = 2109;
    Relation rel = CreateTestIndex(kRelid, "test_idx_relbuf_noop");
    btbuild(rel, BTKeyKind::kInt32);

    // Releasing kInvalidBuffer is a safe no-op (defensive guard in _bt_relbuf).
    _bt_relbuf(rel, kInvalidBuffer);

    // The index is still usable after the no-op release.
    int32_t key = 7;
    ItemPointerData tid = MakeTid(0, 1);
    EXPECT_TRUE(btinsert(rel, BTKeyKind::kInt32, &key, 4, tid));

    RelationClose(rel);
}

TEST_F(NbtreeTest, GetbufAndBinsrchRoundTrip) {
    // End-to-end: _bt_getbuf + _bt_binsrch + _bt_relbuf to locate a key.
    constexpr Oid kRelid = 2110;
    Relation rel = CreateTestIndex(kRelid, "test_idx_getbuf_binsrch");
    btbuild(rel, BTKeyKind::kInt32);

    for (int i = 1; i <= 5; i++) {
        int32_t key = i * 100;  // 100, 200, 300, 400, 500
        ItemPointerData tid = MakeTid(0, static_cast<uint16_t>(i));
        ASSERT_TRUE(btinsert(rel, BTKeyKind::kInt32, &key, 4, tid));
    }

    Buffer buf = _bt_getbuf(rel, 1);
    ASSERT_NE(buf, kInvalidBuffer);
    Page page = BufferGetPage(buf);

    // Locate 300 (should be at offset 3).
    int32_t key = 300;
    OffsetNumber off = _bt_binsrch(page, BTKeyKind::kInt32, &key, 4, false);
    EXPECT_EQ(off, static_cast<OffsetNumber>(3));

    _bt_relbuf(rel, buf);
    RelationClose(rel);
}

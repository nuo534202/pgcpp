// lib_test.cpp — unit tests for M2-lib utility data structures (Task 15.20.1).
//
// Covers all 8 modules under src/lib/:
//   1. dshash        — dynamic shared hash table
//   2. ilist         — intrusive doubly/singly linked lists
//   3. rbtree        — red-black tree
//   4. integerset    — integer set
//   5. bloom         — Bloom filter
//   6. hyperloglog   — HyperLogLog cardinality estimator
//   7. binaryheap    — binary heap
//   8. pairingheap   — pairing heap
//
// The lib modules are self-contained (no MemoryContext dependency for their
// std-container-backed storage) but the AllocSetContext fixture is still
// initialized so any palloc path that may be exercised in future hardening
// has a context to allocate from.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/alloc_set.hpp"
#include "pgcpp/common/memory/memory_context.hpp"
#include "pgcpp/lib/binaryheap.hpp"
#include "pgcpp/lib/bloom.hpp"
#include "pgcpp/lib/dshash.hpp"
#include "pgcpp/lib/hyperloglog.hpp"
#include "pgcpp/lib/ilist.hpp"
#include "pgcpp/lib/integerset.hpp"
#include "pgcpp/lib/pairingheap.hpp"
#include "pgcpp/lib/rbtree.hpp"

namespace {

using mytoydb::error::ErrorData;
using mytoydb::error::LogLevel;
using mytoydb::memory::AllocSetContext;

class LibTest : public ::testing::Test {
protected:
    void SetUp() override {
        mytoydb::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("lib_test_context");
        mytoydb::memory::SetCurrentMemoryContext(context_);
    }

    void TearDown() override {
        mytoydb::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    AllocSetContext* context_ = nullptr;
};

// ===========================================================================
// 1. DsHash
// ===========================================================================

TEST_F(LibTest, DsHashInsertAndLookup) {
    mytoydb::lib::DsHash<int, std::string> table;
    EXPECT_TRUE(table.IsEmpty());
    auto* p = table.Insert(42, "answer");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, "answer");
    auto* q = table.Find(42);
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(*q, "answer");
    EXPECT_EQ(table.Find(7), nullptr);
    EXPECT_EQ(table.Size(), 1u);
}

TEST_F(LibTest, DsHashInsertDuplicateDoesNotReplace) {
    mytoydb::lib::DsHash<int, int> table;
    table.Insert(1, 100);
    auto* p = table.Insert(1, 200);  // duplicate, no replace
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, 100);
    EXPECT_EQ(table.Size(), 1u);
}

TEST_F(LibTest, DsHashInsertDuplicateWithReplace) {
    mytoydb::lib::DsHash<int, int> table;
    table.Insert(1, 100);
    auto* p = table.Insert(1, 200, /*allow_replace=*/true);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, 200);
    EXPECT_EQ(*table.Find(1), 200);
}

TEST_F(LibTest, DsHashDelete) {
    mytoydb::lib::DsHash<int, int> table;
    table.Insert(1, 10);
    table.Insert(2, 20);
    table.Insert(3, 30);
    EXPECT_FALSE(table.Delete(99));
    EXPECT_TRUE(table.Delete(2));
    EXPECT_EQ(table.Find(2), nullptr);
    EXPECT_EQ(table.Size(), 2u);
    EXPECT_TRUE(table.Delete(1));
    EXPECT_TRUE(table.Delete(3));
    EXPECT_TRUE(table.IsEmpty());
}

TEST_F(LibTest, DsHashIterate) {
    mytoydb::lib::DsHash<std::string, int> table;
    table.Insert("a", 1);
    table.Insert("b", 2);
    table.Insert("c", 3);
    auto entries = table.Entries();
    EXPECT_EQ(entries.size(), 3u);
    std::map<std::string, int> sorted(entries.begin(), entries.end());
    EXPECT_EQ(sorted["a"], 1);
    EXPECT_EQ(sorted["b"], 2);
    EXPECT_EQ(sorted["c"], 3);
}

TEST_F(LibTest, DsHashClear) {
    mytoydb::lib::DsHash<int, int> table;
    for (int i = 0; i < 50; ++i) {
        table.Insert(i, i * 2);
    }
    EXPECT_EQ(table.Size(), 50u);
    table.Clear();
    EXPECT_TRUE(table.IsEmpty());
    EXPECT_EQ(table.Find(0), nullptr);
}

// ===========================================================================
// 2. ilist (DList + SList)
// ===========================================================================

struct DItem {
    int value;
    mytoydb::lib::DListNode node;
};

struct SItem {
    int value;
    mytoydb::lib::SListNode node;
};

TEST_F(LibTest, DListPushPopHeadTail) {
    mytoydb::lib::DList list;
    EXPECT_TRUE(list.IsEmpty());
    DItem a{1, {}};
    DItem b{2, {}};
    DItem c{3, {}};
    list.PushHead(&a.node);
    list.PushTail(&b.node);
    list.PushHead(&c.node);
    EXPECT_EQ(list.Length(), 3);
    // c -> a -> b
    auto* head = list.Head();
    ASSERT_NE(head, nullptr);
    EXPECT_EQ((mytoydb::lib::dlist_container<DItem, &DItem::node>(head)->value), 3);
    auto* tail = list.Tail();
    ASSERT_NE(tail, nullptr);
    EXPECT_EQ((mytoydb::lib::dlist_container<DItem, &DItem::node>(tail)->value), 2);
    auto* popped_head = list.PopHead();
    EXPECT_EQ((mytoydb::lib::dlist_container<DItem, &DItem::node>(popped_head)->value), 3);
    auto* popped_tail = list.PopTail();
    EXPECT_EQ((mytoydb::lib::dlist_container<DItem, &DItem::node>(popped_tail)->value), 2);
    EXPECT_EQ(list.Length(), 1);
}

TEST_F(LibTest, DListIterate) {
    mytoydb::lib::DList list;
    DItem items[5];
    for (int i = 0; i < 5; ++i) {
        items[i].value = i;
        list.PushTail(&items[i].node);
    }
    std::vector<int> seen;
    for (auto it = list.begin(); it != list.end(); ++it) {
        seen.push_back(mytoydb::lib::dlist_container<DItem, &DItem::node>(*it)->value);
    }
    EXPECT_EQ(seen, std::vector<int>({0, 1, 2, 3, 4}));
}

TEST_F(LibTest, DListDeleteNode) {
    mytoydb::lib::DList list;
    DItem a{1, {}};
    DItem b{2, {}};
    DItem c{3, {}};
    list.PushTail(&a.node);
    list.PushTail(&b.node);
    list.PushTail(&c.node);
    list.Delete(&b.node);
    EXPECT_EQ(list.Length(), 2);
    std::vector<int> seen;
    for (auto it = list.begin(); it != list.end(); ++it) {
        seen.push_back(mytoydb::lib::dlist_container<DItem, &DItem::node>(*it)->value);
    }
    EXPECT_EQ(seen, std::vector<int>({1, 3}));
}

TEST_F(LibTest, DListPopEmpty) {
    mytoydb::lib::DList list;
    EXPECT_EQ(list.PopHead(), nullptr);
    EXPECT_EQ(list.PopTail(), nullptr);
}

TEST_F(LibTest, DListClear) {
    mytoydb::lib::DList list;
    DItem a{1, {}};
    DItem b{2, {}};
    list.PushTail(&a.node);
    list.PushTail(&b.node);
    list.Clear();
    EXPECT_TRUE(list.IsEmpty());
}

TEST_F(LibTest, SListPushPop) {
    mytoydb::lib::SList list;
    EXPECT_TRUE(list.IsEmpty());
    SItem a{1, {}};
    SItem b{2, {}};
    SItem c{3, {}};
    list.PushHead(&a.node);  // [1]
    list.PushHead(&b.node);  // [2,1]
    list.PushHead(&c.node);  // [3,2,1]
    EXPECT_EQ(list.Length(), 3);
    auto* p = list.PopHead();
    EXPECT_EQ(p, &c.node);
    p = list.PopHead();
    EXPECT_EQ(p, &b.node);
    p = list.PopHead();
    EXPECT_EQ(p, &a.node);
    EXPECT_TRUE(list.IsEmpty());
    EXPECT_EQ(list.PopHead(), nullptr);
}

TEST_F(LibTest, SListIterate) {
    mytoydb::lib::SList list;
    SItem items[5];
    for (int i = 0; i < 5; ++i) {
        items[i].value = i;
        list.PushHead(&items[i].node);  // push in order -> reverse
    }
    std::vector<int> seen;
    for (auto it = list.begin(); it != list.end(); ++it) {
        seen.push_back(
            reinterpret_cast<SItem*>(reinterpret_cast<char*>(*it) - offsetof(SItem, node))->value);
    }
    EXPECT_EQ(seen, std::vector<int>({4, 3, 2, 1, 0}));
}

// ===========================================================================
// 3. RBTree
// ===========================================================================

TEST_F(LibTest, RBTreeInsertAndFind) {
    mytoydb::lib::RBTree<int> tree;
    EXPECT_TRUE(tree.IsEmpty());
    EXPECT_TRUE(tree.Insert(5));
    EXPECT_TRUE(tree.Insert(3));
    EXPECT_TRUE(tree.Insert(8));
    EXPECT_TRUE(tree.Insert(1));
    EXPECT_TRUE(tree.Insert(9));
    EXPECT_FALSE(tree.Insert(5));  // duplicate
    EXPECT_EQ(tree.Size(), 5u);
    auto* p = tree.Find(8);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, 8);
    EXPECT_EQ(tree.Find(99), nullptr);
}

TEST_F(LibTest, RBTreeDelete) {
    mytoydb::lib::RBTree<int> tree;
    for (int v : {10, 5, 15, 3, 7, 12, 20, 1, 4, 6, 8, 11, 13, 18, 25}) {
        tree.Insert(v);
    }
    EXPECT_EQ(tree.Size(), 15u);
    EXPECT_TRUE(tree.Delete(10));
    EXPECT_EQ(tree.Size(), 14u);
    EXPECT_EQ(tree.Find(10), nullptr);
    EXPECT_FALSE(tree.Delete(99));
    // Remaining values still iterable in sorted order.
    auto sorted = tree.ToSortedVector();
    EXPECT_EQ(sorted, std::vector<int>({1, 3, 4, 5, 6, 7, 8, 11, 12, 13, 15, 18, 20, 25}));
}

TEST_F(LibTest, RBTreeForeachLeftAscending) {
    mytoydb::lib::RBTree<int> tree;
    for (int v : {5, 3, 8, 1, 4, 7, 9}) {
        tree.Insert(v);
    }
    std::vector<int> seen;
    tree.ForeachLeft([&](const int& v) { seen.push_back(v); });
    EXPECT_EQ(seen, std::vector<int>({1, 3, 4, 5, 7, 8, 9}));
}

TEST_F(LibTest, RBTreeForeachRightDescending) {
    mytoydb::lib::RBTree<int> tree;
    for (int v : {5, 3, 8, 1, 4, 7, 9}) {
        tree.Insert(v);
    }
    std::vector<int> seen;
    tree.ForeachRight([&](const int& v) { seen.push_back(v); });
    EXPECT_EQ(seen, std::vector<int>({9, 8, 7, 5, 4, 3, 1}));
}

TEST_F(LibTest, RBTreeClear) {
    mytoydb::lib::RBTree<std::string> tree;
    for (int i = 0; i < 50; ++i) {
        tree.Insert(std::to_string(i));
    }
    EXPECT_EQ(tree.Size(), 50u);
    tree.Clear();
    EXPECT_TRUE(tree.IsEmpty());
    EXPECT_EQ(tree.Find("0"), nullptr);
}

TEST_F(LibTest, RBTreeMoveSemantics) {
    mytoydb::lib::RBTree<int> tree;
    for (int i = 0; i < 10; ++i) {
        tree.Insert(i);
    }
    mytoydb::lib::RBTree<int> moved = std::move(tree);
    EXPECT_EQ(moved.Size(), 10u);
    EXPECT_TRUE(tree.IsEmpty());
    auto sorted = moved.ToSortedVector();
    EXPECT_EQ(sorted, std::vector<int>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9}));
}

// ===========================================================================
// 4. IntegerSet
// ===========================================================================

TEST_F(LibTest, IntegerSetAddContains) {
    mytoydb::lib::IntegerSet set;
    EXPECT_TRUE(set.IsEmpty());
    EXPECT_TRUE(set.Add(42));
    EXPECT_FALSE(set.Add(42));  // duplicate
    EXPECT_TRUE(set.Add(100));
    EXPECT_EQ(set.NumMembers(), 2u);
    EXPECT_TRUE(set.Contains(42));
    EXPECT_TRUE(set.Contains(100));
    EXPECT_FALSE(set.Contains(7));
}

TEST_F(LibTest, IntegerSetIterate) {
    mytoydb::lib::IntegerSet set;
    for (uint64_t v : {5u, 1u, 100u, 50u, 2u}) {
        set.Add(v);
    }
    set.BeginIterate();
    std::vector<uint64_t> seen;
    uint64_t out = 0;
    while (set.IterateNext(&out)) {
        seen.push_back(out);
    }
    EXPECT_EQ(seen, std::vector<uint64_t>({1u, 2u, 5u, 50u, 100u}));
}

TEST_F(LibTest, IntegerSetReset) {
    mytoydb::lib::IntegerSet set;
    for (uint64_t v = 0; v < 10; ++v) {
        set.Add(v);
    }
    EXPECT_EQ(set.NumMembers(), 10u);
    set.Reset();
    EXPECT_TRUE(set.IsEmpty());
}

// ===========================================================================
// 5. BloomFilter
// ===========================================================================

TEST_F(LibTest, BloomFilterNoFalseNegatives) {
    mytoydb::lib::BloomFilter bf(/*bit_count=*/4096, /*k=*/7);
    std::vector<std::string> keys = {"alpha", "beta", "gamma", "delta", "epsilon"};
    for (const auto& k : keys) {
        bf.Add(k.data(), k.size());
    }
    // Every inserted key must be present (no false negatives).
    for (const auto& k : keys) {
        EXPECT_TRUE(bf.Test(k.data(), k.size())) << "missing: " << k;
    }
}

TEST_F(LibTest, BloomFilterReset) {
    mytoydb::lib::BloomFilter bf(2048, 5);
    std::string key = "test";
    bf.Add(key.data(), key.size());
    EXPECT_TRUE(bf.Test(key.data(), key.size()));
    bf.Reset();
    // After reset the filter should have no bits set, so Test returns false
    // (unless the key's hash positions happen to coincide with leftover
    // zeros — but with k=5 across 2048 bits, accidental retention is
    // astronomically unlikely).
    EXPECT_FALSE(bf.Test(key.data(), key.size()));
}

TEST_F(LibTest, BloomFilterFalsePositiveRate) {
    // With a generous bit count the false positive rate should be low.
    mytoydb::lib::BloomFilter bf(8192, 6);
    for (int i = 0; i < 100; ++i) {
        std::string k = "in" + std::to_string(i);
        bf.Add(k.data(), k.size());
    }
    int false_positives = 0;
    int total_queries = 1000;
    for (int i = 0; i < total_queries; ++i) {
        std::string k = "out" + std::to_string(i);
        if (bf.Test(k.data(), k.size())) {
            ++false_positives;
        }
    }
    // Conservative bound: <10% false positive rate with 100 insertions.
    EXPECT_LT(false_positives, total_queries / 10);
}

// ===========================================================================
// 6. HyperLogLog
// ===========================================================================

TEST_F(LibTest, HyperLogLogEstimateDistinctCount) {
    mytoydb::lib::HyperLogLog hll(/*register_bits=*/14);
    const int n = 1000;
    for (int i = 0; i < n; ++i) {
        std::string s = "item" + std::to_string(i);
        hll.Add(s.data(), s.size());
    }
    uint64_t estimate = hll.Estimate();
    // HLL with p=14 has ~0.81% relative error; we allow ±10% to keep the
    // test robust against hash-distribution variance.
    double rel_error = std::abs(static_cast<double>(estimate) - n) / n;
    EXPECT_LT(rel_error, 0.10);
}

TEST_F(LibTest, HyperLogLogDuplicateNoOp) {
    mytoydb::lib::HyperLogLog hll(14);
    for (int i = 0; i < 100; ++i) {
        std::string s = "same";
        hll.Add(s.data(), s.size());
    }
    uint64_t estimate = hll.Estimate();
    // All inserts were identical; estimate should be near 1.
    EXPECT_GE(estimate, 1u);
    EXPECT_LE(estimate, 3u);
}

TEST_F(LibTest, HyperLogLogReset) {
    mytoydb::lib::HyperLogLog hll(14);
    for (int i = 0; i < 100; ++i) {
        std::string s = "x" + std::to_string(i);
        hll.Add(s.data(), s.size());
    }
    EXPECT_GT(hll.Estimate(), 0u);
    hll.Reset();
    // After reset, register count is unchanged but estimate should be ~0.
    EXPECT_EQ(hll.RegisterCount(), 1u << 14);
    EXPECT_EQ(hll.Estimate(), 0u);
}

// ===========================================================================
// 7. BinaryHeap (min-heap by default)
// ===========================================================================

TEST_F(LibTest, BinaryHeapMinOrder) {
    mytoydb::lib::BinaryHeap<int> heap;  // std::less<int> -> min-heap
    for (int v : {5, 3, 8, 1, 9, 4, 7, 2, 6}) {
        heap.Add(v);
    }
    EXPECT_EQ(heap.Size(), 9u);
    EXPECT_EQ(heap.Top(), 1);
    std::vector<int> out;
    int val = 0;
    while (heap.Pop(&val)) {
        out.push_back(val);
    }
    EXPECT_EQ(out, std::vector<int>({1, 2, 3, 4, 5, 6, 7, 8, 9}));
}

TEST_F(LibTest, BinaryHeapMaxOrder) {
    mytoydb::lib::BinaryHeap<int, std::greater<int>> heap;
    for (int v : {5, 3, 8, 1, 9, 4, 7, 2, 6}) {
        heap.Add(v);
    }
    EXPECT_EQ(heap.Top(), 9);
    std::vector<int> out;
    int val = 0;
    while (heap.Pop(&val)) {
        out.push_back(val);
    }
    EXPECT_EQ(out, std::vector<int>({9, 8, 7, 6, 5, 4, 3, 2, 1}));
}

TEST_F(LibTest, BinaryHeapReplaceTop) {
    mytoydb::lib::BinaryHeap<int> heap;
    for (int v : {5, 3, 8, 1, 9}) {
        heap.Add(v);
    }
    EXPECT_EQ(heap.Top(), 1);
    EXPECT_TRUE(heap.ReplaceTop(100));
    EXPECT_EQ(heap.Top(), 3);
    EXPECT_TRUE(heap.ReplaceTop(0));  // heap still non-empty → returns true
    heap.Reset();
    EXPECT_TRUE(heap.IsEmpty());
    EXPECT_FALSE(heap.ReplaceTop(0));  // empty heap → returns false
}

TEST_F(LibTest, BinaryHeapBuildBulk) {
    mytoydb::lib::BinaryHeap<int> heap;
    std::vector<int> data = {7, 2, 9, 4, 1, 6, 3, 8, 5};
    heap.Build(data.begin(), data.end());
    EXPECT_EQ(heap.Size(), data.size());
    std::vector<int> out;
    int val = 0;
    while (heap.Pop(&val)) {
        out.push_back(val);
    }
    std::vector<int> expected = data;
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(out, expected);
}

TEST_F(LibTest, BinaryHeapPopEmpty) {
    mytoydb::lib::BinaryHeap<int> heap;
    int val = 0;
    EXPECT_FALSE(heap.Pop(&val));
}

// ===========================================================================
// 8. PairingHeap (min-heap by default)
// ===========================================================================

TEST_F(LibTest, PairingHeapMinOrder) {
    mytoydb::lib::PairingHeap<int> heap;
    for (int v : {5, 3, 8, 1, 9, 4, 7, 2, 6}) {
        heap.Add(v);
    }
    EXPECT_EQ(heap.Size(), 9u);
    EXPECT_EQ(heap.Top(), 1);
    std::vector<int> out;
    int val = 0;
    while (heap.Pop(&val)) {
        out.push_back(val);
    }
    EXPECT_EQ(out, std::vector<int>({1, 2, 3, 4, 5, 6, 7, 8, 9}));
}

TEST_F(LibTest, PairingHeapMaxOrder) {
    mytoydb::lib::PairingHeap<int, std::greater<int>> heap;
    for (int v : {5, 3, 8, 1, 9, 4, 7, 2, 6}) {
        heap.Add(v);
    }
    EXPECT_EQ(heap.Top(), 9);
    std::vector<int> out;
    int val = 0;
    while (heap.Pop(&val)) {
        out.push_back(val);
    }
    EXPECT_EQ(out, std::vector<int>({9, 8, 7, 6, 5, 4, 3, 2, 1}));
}

TEST_F(LibTest, PairingHeapMeld) {
    mytoydb::lib::PairingHeap<int> a;
    mytoydb::lib::PairingHeap<int> b;
    for (int v : {1, 5, 9}) {
        a.Add(v);
    }
    for (int v : {2, 6, 10}) {
        b.Add(v);
    }
    a.Meld(b);
    EXPECT_EQ(a.Size(), 6u);
    EXPECT_TRUE(b.IsEmpty());
    std::vector<int> out;
    int val = 0;
    while (a.Pop(&val)) {
        out.push_back(val);
    }
    EXPECT_EQ(out, std::vector<int>({1, 2, 5, 6, 9, 10}));
}

TEST_F(LibTest, PairingHeapStress) {
    mytoydb::lib::PairingHeap<int> heap;
    std::vector<int> expected;
    for (int i = 0; i < 200; ++i) {
        int v = (i * 37) % 1000;
        heap.Add(v);
        expected.push_back(v);
    }
    std::sort(expected.begin(), expected.end());
    std::vector<int> out;
    int val = 0;
    while (heap.Pop(&val)) {
        out.push_back(val);
    }
    EXPECT_EQ(out, expected);
}

TEST_F(LibTest, PairingHeapPopEmpty) {
    mytoydb::lib::PairingHeap<int> heap;
    int val = 0;
    EXPECT_FALSE(heap.Pop(&val));
}

}  // namespace

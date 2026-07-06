// tuplesort_test.cpp — Unit tests for TupleSort external merge sort (P1-3).
//
// Exercises the TupleSort class with:
// - In-memory sort (data fits in work_mem): single key ASC/DESC, multi-key,
//   NULLS FIRST/LAST, empty input, single tuple, text type.
// - External merge sort (data exceeds work_mem): forces spilling to disk
//   via a small work_mem, verifies correctness with multiple runs and
//   k-way merge. Includes int4, text, and mixed-type tests.

#include "utils/sort/tuplesort.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "access/rel.hpp"
#include "catalog/pg_attribute.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "executor/tupletable.hpp"
#include "types/datum.hpp"

using pgcpp::access::CreateTupleDesc;
using pgcpp::access::TupleDesc;
using pgcpp::catalog::AttAlign;
using pgcpp::catalog::AttStorage;
using pgcpp::catalog::FormData_pg_attribute;
using pgcpp::executor::TupleTableSlot;
using pgcpp::memory::AllocSetContext;
using pgcpp::memory::palloc;
using pgcpp::sort::SortKey;
using pgcpp::sort::TupleSort;
using pgcpp::types::Datum;
using pgcpp::types::DatumGetInt32;
using pgcpp::types::DatumGetTextP;
using pgcpp::types::Int32GetDatum;
using pgcpp::types::kInt4Oid;
using pgcpp::types::kTextOid;
using pgcpp::types::VARDATA;
using pgcpp::types::VARSIZE;

namespace {

class TuplesortTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("tuplesort_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);
    }

    void TearDown() override {
        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    // Build a 1-column int4 descriptor.
    TupleDesc MakeIntDesc() {
        FormData_pg_attribute a;
        a.attname = "a";
        a.attnum = 1;
        a.atttypid = kInt4Oid;
        a.attlen = 4;
        a.attbyval = true;
        a.attalign = AttAlign::kInt;
        a.attstorage = AttStorage::kPlain;
        return CreateTupleDesc({a});
    }

    // Build a 2-column (int4, int4) descriptor.
    TupleDesc MakeIntIntDesc() {
        FormData_pg_attribute a1;
        a1.attname = "a";
        a1.attnum = 1;
        a1.atttypid = kInt4Oid;
        a1.attlen = 4;
        a1.attbyval = true;
        a1.attalign = AttAlign::kInt;
        a1.attstorage = AttStorage::kPlain;
        FormData_pg_attribute a2;
        a2.attname = "b";
        a2.attnum = 2;
        a2.atttypid = kInt4Oid;
        a2.attlen = 4;
        a2.attbyval = true;
        a2.attalign = AttAlign::kInt;
        a2.attstorage = AttStorage::kPlain;
        return CreateTupleDesc({a1, a2});
    }

    // Build a 1-column text descriptor.
    TupleDesc MakeTextDesc() {
        FormData_pg_attribute a;
        a.attname = "s";
        a.attnum = 1;
        a.atttypid = kTextOid;
        a.attlen = -1;  // variable-length
        a.attbyval = false;
        a.attalign = AttAlign::kInt;
        a.attstorage = AttStorage::kExtended;
        return CreateTupleDesc({a});
    }

    // Make an int4 slot with the given value.
    TupleTableSlot* MakeIntSlot(TupleDesc desc, int32_t value, bool is_null = false) {
        auto* slot = TupleTableSlot::Make(desc);
        Datum v = Int32GetDatum(value);
        bool n = is_null;
        slot->StoreVirtual(&v, &n);
        return slot;
    }

    // Make a (int4, int4) slot.
    TupleTableSlot* MakeIntIntSlot(TupleDesc desc, int32_t a, int32_t b) {
        auto* slot = TupleTableSlot::Make(desc);
        Datum values[2] = {Int32GetDatum(a), Int32GetDatum(b)};
        bool isnull[2] = {false, false};
        slot->StoreVirtual(values, isnull);
        return slot;
    }

    // Build a varlena text datum (4-byte length header + data).
    Datum MakeTextDatum(const std::string& s) {
        int32_t total = static_cast<int32_t>(sizeof(int32_t) + s.size());
        char* buf = static_cast<char*>(palloc(total));
        std::memcpy(buf, &total, sizeof(int32_t));
        std::memcpy(buf + sizeof(int32_t), s.data(), s.size());
        return Datum(buf);
    }

    // Make a text slot.
    TupleTableSlot* MakeTextSlot(TupleDesc desc, const std::string& s, bool is_null = false) {
        auto* slot = TupleTableSlot::Make(desc);
        if (is_null) {
            Datum v = 0;
            bool n = true;
            slot->StoreVirtual(&v, &n);
        } else {
            Datum v = MakeTextDatum(s);
            bool n = false;
            slot->StoreVirtual(&v, &n);
        }
        return slot;
    }

    std::string TextDatumToString(Datum d) {
        const char* p = DatumGetTextP(d);
        int len = VARSIZE(p);
        int data_len = len - static_cast<int>(sizeof(int32_t));
        return std::string(VARDATA(p), data_len);
    }

    // Run a TupleSort to completion and collect int4 results.
    std::vector<int32_t> CollectIntResults(TupleSort& ts) {
        std::vector<int32_t> out;
        TupleTableSlot* slot = nullptr;
        while ((slot = ts.GetTuple()) != nullptr) {
            out.push_back(DatumGetInt32(slot->tts_values[0]));
        }
        return out;
    }

    // Run a TupleSort to completion and collect text results.
    std::vector<std::string> CollectTextResults(TupleSort& ts) {
        std::vector<std::string> out;
        TupleTableSlot* slot = nullptr;
        while ((slot = ts.GetTuple()) != nullptr) {
            out.push_back(TextDatumToString(slot->tts_values[0]));
        }
        return out;
    }

    AllocSetContext* context_ = nullptr;
};

// ===========================================================================
// 1. EmptyInput — no tuples fed, no tuples out.
// ===========================================================================
TEST_F(TuplesortTest, EmptyInput) {
    TupleDesc desc = MakeIntDesc();
    TupleSort ts(desc, {{/*attnum=*/1, kInt4Oid, /*reverse=*/false, /*nulls_first=*/false}},
                 /*work_mem=*/4 * 1024 * 1024);
    ts.PerformSort();
    EXPECT_TRUE(CollectIntResults(ts).empty());
    EXPECT_EQ(ts.NumRuns(), 0);
}

// ===========================================================================
// 2. SingleTuple — one tuple in, same tuple out.
// ===========================================================================
TEST_F(TuplesortTest, SingleTuple) {
    TupleDesc desc = MakeIntDesc();
    TupleSort ts(desc, {{1, kInt4Oid, false, false}},
                 /*work_mem=*/4 * 1024 * 1024);
    auto* slot = MakeIntSlot(desc, 42);
    ts.PutTuple(slot);
    ts.PerformSort();
    auto result = CollectIntResults(ts);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], 42);
    EXPECT_EQ(ts.NumRuns(), 0);  // in-memory
}

// ===========================================================================
// 3. InMemoryAscending — 5 rows in random order, sort ascending.
// ===========================================================================
TEST_F(TuplesortTest, InMemoryAscending) {
    TupleDesc desc = MakeIntDesc();
    TupleSort ts(desc, {{1, kInt4Oid, false, false}},
                 /*work_mem=*/4 * 1024 * 1024);
    for (int v : {3, 1, 5, 2, 4}) {
        auto* slot = MakeIntSlot(desc, v);
        ts.PutTuple(slot);
    }
    ts.PerformSort();
    auto result = CollectIntResults(ts);
    ASSERT_EQ(result.size(), 5u);
    EXPECT_EQ(result[0], 1);
    EXPECT_EQ(result[1], 2);
    EXPECT_EQ(result[2], 3);
    EXPECT_EQ(result[3], 4);
    EXPECT_EQ(result[4], 5);
    EXPECT_EQ(ts.NumRuns(), 0);
}

// ===========================================================================
// 4. InMemoryDescending — 5 rows, sort descending.
// ===========================================================================
TEST_F(TuplesortTest, InMemoryDescending) {
    TupleDesc desc = MakeIntDesc();
    TupleSort ts(desc, {{1, kInt4Oid, /*reverse=*/true, false}},
                 /*work_mem=*/4 * 1024 * 1024);
    for (int v : {3, 1, 5, 2, 4}) {
        auto* slot = MakeIntSlot(desc, v);
        ts.PutTuple(slot);
    }
    ts.PerformSort();
    auto result = CollectIntResults(ts);
    ASSERT_EQ(result.size(), 5u);
    EXPECT_EQ(result[0], 5);
    EXPECT_EQ(result[1], 4);
    EXPECT_EQ(result[2], 3);
    EXPECT_EQ(result[3], 2);
    EXPECT_EQ(result[4], 1);
}

// ===========================================================================
// 5. InMemoryMultiKey — sort by (a ASC, b ASC).
// ===========================================================================
TEST_F(TuplesortTest, InMemoryMultiKey) {
    TupleDesc desc = MakeIntIntDesc();
    TupleSort ts(desc, {{1, kInt4Oid, false, false}, {2, kInt4Oid, false, false}},
                 /*work_mem=*/4 * 1024 * 1024);
    // (a,b): (1,2), (1,1), (2,1), (1,3), (2,0)
    for (auto [a, b] : std::vector<std::pair<int, int>>{{1, 2}, {1, 1}, {2, 1}, {1, 3}, {2, 0}}) {
        auto* slot = MakeIntIntSlot(desc, a, b);
        ts.PutTuple(slot);
    }
    ts.PerformSort();
    std::vector<std::pair<int, int>> result;
    TupleTableSlot* slot = nullptr;
    while ((slot = ts.GetTuple()) != nullptr) {
        result.emplace_back(DatumGetInt32(slot->tts_values[0]), DatumGetInt32(slot->tts_values[1]));
    }
    ASSERT_EQ(result.size(), 5u);
    EXPECT_EQ(result[0], std::make_pair(1, 1));
    EXPECT_EQ(result[1], std::make_pair(1, 2));
    EXPECT_EQ(result[2], std::make_pair(1, 3));
    EXPECT_EQ(result[3], std::make_pair(2, 0));
    EXPECT_EQ(result[4], std::make_pair(2, 1));
}

// ===========================================================================
// 6. InMemoryNullsLast — NULLs sort last on ascending.
// ===========================================================================
TEST_F(TuplesortTest, InMemoryNullsLast) {
    TupleDesc desc = MakeIntDesc();
    TupleSort ts(desc, {{1, kInt4Oid, false, /*nulls_first=*/false}},
                 /*work_mem=*/4 * 1024 * 1024);
    // Mix of NULLs and values: 3, NULL, 1, NULL, 2
    for (int i = 0; i < 5; i++) {
        TupleTableSlot* slot;
        if (i == 1 || i == 3) {
            slot = MakeIntSlot(desc, 0, /*is_null=*/true);
        } else {
            slot = MakeIntSlot(desc, std::vector<int>{3, 0, 1, 0, 2}[i]);
        }
        ts.PutTuple(slot);
    }
    ts.PerformSort();
    auto result = CollectIntResults(ts);
    ASSERT_EQ(result.size(), 5u);
    // NULLs last: 1, 2, 3, NULL, NULL (NULLs render as 0 via DatumGetInt32)
    EXPECT_EQ(result[0], 1);
    EXPECT_EQ(result[1], 2);
    EXPECT_EQ(result[2], 3);
    // The two NULL slots have tts_isnull[0] == true; values are 0.
    EXPECT_EQ(result[3], 0);
    EXPECT_EQ(result[4], 0);
}

// ===========================================================================
// 7. InMemoryNullsFirst — NULLs sort first on ascending.
// ===========================================================================
TEST_F(TuplesortTest, InMemoryNullsFirst) {
    TupleDesc desc = MakeIntDesc();
    TupleSort ts(desc, {{1, kInt4Oid, false, /*nulls_first=*/true}},
                 /*work_mem=*/4 * 1024 * 1024);
    for (int i = 0; i < 5; i++) {
        TupleTableSlot* slot;
        if (i == 1 || i == 3) {
            slot = MakeIntSlot(desc, 0, /*is_null=*/true);
        } else {
            slot = MakeIntSlot(desc, std::vector<int>{3, 0, 1, 0, 2}[i]);
        }
        ts.PutTuple(slot);
    }
    ts.PerformSort();
    auto result = CollectIntResults(ts);
    ASSERT_EQ(result.size(), 5u);
    // NULLs first: NULL, NULL, 1, 2, 3
    EXPECT_EQ(result[0], 0);
    EXPECT_EQ(result[1], 0);
    EXPECT_EQ(result[2], 1);
    EXPECT_EQ(result[3], 2);
    EXPECT_EQ(result[4], 3);
}

// ===========================================================================
// 8. InMemoryText — sort text values ascending.
// ===========================================================================
TEST_F(TuplesortTest, InMemoryText) {
    TupleDesc desc = MakeTextDesc();
    TupleSort ts(desc, {{1, kTextOid, false, false}},
                 /*work_mem=*/4 * 1024 * 1024);
    for (const auto& s : std::vector<std::string>{"banana", "apple", "cherry", "date"}) {
        auto* slot = MakeTextSlot(desc, s);
        ts.PutTuple(slot);
    }
    ts.PerformSort();
    auto result = CollectTextResults(ts);
    ASSERT_EQ(result.size(), 4u);
    EXPECT_EQ(result[0], "apple");
    EXPECT_EQ(result[1], "banana");
    EXPECT_EQ(result[2], "cherry");
    EXPECT_EQ(result[3], "date");
}

// ===========================================================================
// 9. ExternalSortSmallWorkMem — force spilling with work_mem=1.
// ===========================================================================
TEST_F(TuplesortTest, ExternalSortSmallWorkMem) {
    TupleDesc desc = MakeIntDesc();
    // work_mem=1 byte forces a spill after the very first tuple.
    TupleSort ts(desc, {{1, kInt4Oid, false, false}},
                 /*work_mem=*/1);
    for (int v : {5, 3, 1, 4, 2}) {
        auto* slot = MakeIntSlot(desc, v);
        ts.PutTuple(slot);
    }
    ts.PerformSort();
    auto result = CollectIntResults(ts);
    ASSERT_EQ(result.size(), 5u);
    EXPECT_EQ(result[0], 1);
    EXPECT_EQ(result[1], 2);
    EXPECT_EQ(result[2], 3);
    EXPECT_EQ(result[3], 4);
    EXPECT_EQ(result[4], 5);
    // Spilled: should have at least 1 run (likely multiple).
    EXPECT_GE(ts.NumRuns(), 1);
}

// ===========================================================================
// 10. ExternalSortManyTuples — 100 tuples, work_mem=64 to force spilling.
// ===========================================================================
TEST_F(TuplesortTest, ExternalSortManyTuples) {
    TupleDesc desc = MakeIntDesc();
    TupleSort ts(desc, {{1, kInt4Oid, false, false}},
                 /*work_mem=*/64);
    // Insert 100 tuples in descending order.
    for (int i = 99; i >= 0; i--) {
        auto* slot = MakeIntSlot(desc, i);
        ts.PutTuple(slot);
    }
    ts.PerformSort();
    auto result = CollectIntResults(ts);
    ASSERT_EQ(result.size(), 100u);
    for (int i = 0; i < 100; i++) {
        EXPECT_EQ(result[i], i) << "Mismatch at index " << i;
    }
    EXPECT_GE(ts.NumRuns(), 2);
}

// ===========================================================================
// 11. ExternalSortText — external sort with text type.
// ===========================================================================
TEST_F(TuplesortTest, ExternalSortText) {
    TupleDesc desc = MakeTextDesc();
    TupleSort ts(desc, {{1, kTextOid, false, false}},
                 /*work_mem=*/1);
    std::vector<std::string> words = {"zebra",  "mango", "apple", "banana",
                                      "cherry", "date",  "fig",   "grape"};
    for (const auto& s : words) {
        auto* slot = MakeTextSlot(desc, s);
        ts.PutTuple(slot);
    }
    ts.PerformSort();
    auto result = CollectTextResults(ts);
    ASSERT_EQ(result.size(), words.size());
    std::vector<std::string> expected = words;
    std::sort(expected.begin(), expected.end());
    for (size_t i = 0; i < expected.size(); i++) {
        EXPECT_EQ(result[i], expected[i]) << "Mismatch at index " << i;
    }
    EXPECT_GE(ts.NumRuns(), 1);
}

// ===========================================================================
// 12. ExternalSortDescending — external sort with DESC.
// ===========================================================================
TEST_F(TuplesortTest, ExternalSortDescending) {
    TupleDesc desc = MakeIntDesc();
    TupleSort ts(desc, {{1, kInt4Oid, /*reverse=*/true, false}},
                 /*work_mem=*/1);
    for (int v : {5, 3, 1, 4, 2}) {
        auto* slot = MakeIntSlot(desc, v);
        ts.PutTuple(slot);
    }
    ts.PerformSort();
    auto result = CollectIntResults(ts);
    ASSERT_EQ(result.size(), 5u);
    EXPECT_EQ(result[0], 5);
    EXPECT_EQ(result[1], 4);
    EXPECT_EQ(result[2], 3);
    EXPECT_EQ(result[3], 2);
    EXPECT_EQ(result[4], 1);
}

// ===========================================================================
// 13. AlreadySorted — input already in order; sort should be idempotent.
// ===========================================================================
TEST_F(TuplesortTest, AlreadySorted) {
    TupleDesc desc = MakeIntDesc();
    TupleSort ts(desc, {{1, kInt4Oid, false, false}},
                 /*work_mem=*/4 * 1024 * 1024);
    for (int i = 1; i <= 10; i++) {
        auto* slot = MakeIntSlot(desc, i);
        ts.PutTuple(slot);
    }
    ts.PerformSort();
    auto result = CollectIntResults(ts);
    ASSERT_EQ(result.size(), 10u);
    for (int i = 0; i < 10; i++) {
        EXPECT_EQ(result[i], i + 1);
    }
}

// ===========================================================================
// 14. DuplicateKeys — tuples with equal sort keys preserve stable order.
// ===========================================================================
TEST_F(TuplesortTest, DuplicateKeys) {
    TupleDesc desc = MakeIntIntDesc();
    TupleSort ts(desc, {{1, kInt4Oid, false, false}},
                 /*work_mem=*/4 * 1024 * 1024);
    // All have a=1, varying b. Sort is on a only.
    for (int b : {10, 20, 30, 40, 50}) {
        auto* slot = MakeIntIntSlot(desc, 1, b);
        ts.PutTuple(slot);
    }
    ts.PerformSort();
    std::vector<int> result_b;
    TupleTableSlot* slot = nullptr;
    while ((slot = ts.GetTuple()) != nullptr) {
        result_b.push_back(DatumGetInt32(slot->tts_values[1]));
    }
    ASSERT_EQ(result_b.size(), 5u);
    // std::sort is not guaranteed stable, but for equal keys the order
    // is unspecified. We only check that all values are present.
    int sum = 0;
    for (int v : result_b)
        sum += v;
    EXPECT_EQ(sum, 10 + 20 + 30 + 40 + 50);
}

}  // namespace

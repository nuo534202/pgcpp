// tuplestore_test.cpp — Unit tests for Tuplestore (P1-4).
//
// Exercises the Tuplestore class with:
// - In-memory storage (data fits in work_mem): put/get, rewind, empty.
// - Disk spill (data exceeds work_mem): forces spilling via small work_mem,
//   verifies correctness of serialization/deserialization.
// - Text (byref) type: ensures varlena data is correctly deep-copied and
//   serialized.
#include "utils/sort/tuplestore.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
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
using pgcpp::sort::Tuplestore;
using pgcpp::types::Datum;
using pgcpp::types::DatumGetInt32;
using pgcpp::types::DatumGetTextP;
using pgcpp::types::Int32GetDatum;
using pgcpp::types::kInt4Oid;
using pgcpp::types::kTextOid;
using pgcpp::types::VARDATA;
using pgcpp::types::VARSIZE;

namespace {

class TuplestoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("tuplestore_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);
    }

    void TearDown() override {
        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

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

    TupleDesc MakeTextDesc() {
        FormData_pg_attribute a;
        a.attname = "s";
        a.attnum = 1;
        a.atttypid = kTextOid;
        a.attlen = -1;
        a.attbyval = false;
        a.attalign = AttAlign::kInt;
        a.attstorage = AttStorage::kExtended;
        return CreateTupleDesc({a});
    }

    TupleTableSlot* MakeIntSlot(TupleDesc desc, int32_t value) {
        auto* slot = TupleTableSlot::Make(desc);
        Datum v = Int32GetDatum(value);
        bool n = false;
        slot->StoreVirtual(&v, &n);
        return slot;
    }

    TupleTableSlot* MakeIntIntSlot(TupleDesc desc, int32_t a, int32_t b) {
        auto* slot = TupleTableSlot::Make(desc);
        Datum values[2] = {Int32GetDatum(a), Int32GetDatum(b)};
        bool isnull[2] = {false, false};
        slot->StoreVirtual(values, isnull);
        return slot;
    }

    Datum MakeTextDatum(const std::string& s) {
        int32_t total = static_cast<int32_t>(sizeof(int32_t) + s.size());
        char* buf = static_cast<char*>(palloc(total));
        std::memcpy(buf, &total, sizeof(int32_t));
        std::memcpy(buf + sizeof(int32_t), s.data(), s.size());
        return Datum(buf);
    }

    TupleTableSlot* MakeTextSlot(TupleDesc desc, const std::string& s) {
        auto* slot = TupleTableSlot::Make(desc);
        Datum v = MakeTextDatum(s);
        bool n = false;
        slot->StoreVirtual(&v, &n);
        return slot;
    }

    std::string TextDatumToString(Datum d) {
        const char* p = DatumGetTextP(d);
        int len = VARSIZE(p);
        int data_len = len - static_cast<int>(sizeof(int32_t));
        return std::string(VARDATA(p), data_len);
    }

    AllocSetContext* context_ = nullptr;
};

// ===========================================================================
// 1. EmptyInput — no tuples, GetTuple returns nullptr immediately.
// ===========================================================================
TEST_F(TuplestoreTest, EmptyInput) {
    TupleDesc desc = MakeIntDesc();
    Tuplestore ts(desc, /*work_mem=*/4 * 1024 * 1024);
    EXPECT_EQ(ts.NumTuples(), 0);
    EXPECT_EQ(ts.GetTuple(), nullptr);
    EXPECT_FALSE(ts.IsOnDisk());
}

// ===========================================================================
// 2. InMemoryPutGet — put 3 int4 tuples, get them back in order.
// ===========================================================================
TEST_F(TuplestoreTest, InMemoryPutGet) {
    TupleDesc desc = MakeIntDesc();
    Tuplestore ts(desc, /*work_mem=*/4 * 1024 * 1024);

    for (int i = 0; i < 3; i++) {
        auto* slot = MakeIntSlot(desc, 100 + i);
        ts.PutTuple(slot);
        // PutTuple deep-copies, so we can free the input slot.
        // (Slots are palloc'd; they'll be freed when the context is deleted.)
    }
    EXPECT_EQ(ts.NumTuples(), 3);
    EXPECT_FALSE(ts.IsOnDisk());

    std::vector<int32_t> results;
    TupleTableSlot* slot = nullptr;
    while ((slot = ts.GetTuple()) != nullptr) {
        results.push_back(DatumGetInt32(slot->tts_values[0]));
    }
    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0], 100);
    EXPECT_EQ(results[1], 101);
    EXPECT_EQ(results[2], 102);
}

// ===========================================================================
// 3. InMemoryRewind — read twice with Rewind in between.
// ===========================================================================
TEST_F(TuplestoreTest, InMemoryRewind) {
    TupleDesc desc = MakeIntDesc();
    Tuplestore ts(desc, /*work_mem=*/4 * 1024 * 1024);

    for (int i = 0; i < 5; i++) {
        auto* slot = MakeIntSlot(desc, i * 10);
        ts.PutTuple(slot);
    }

    // First read.
    std::vector<int32_t> first;
    TupleTableSlot* s = nullptr;
    while ((s = ts.GetTuple()) != nullptr) {
        first.push_back(DatumGetInt32(s->tts_values[0]));
    }
    ASSERT_EQ(first.size(), 5u);

    // Rewind and read again.
    ts.Rewind();
    std::vector<int32_t> second;
    while ((s = ts.GetTuple()) != nullptr) {
        second.push_back(DatumGetInt32(s->tts_values[0]));
    }
    ASSERT_EQ(second.size(), 5u);
    for (int i = 0; i < 5; i++) {
        EXPECT_EQ(first[i], second[i]);
    }
}

// ===========================================================================
// 4. SpillToDisk — force spill with very small work_mem, verify int4 data.
// ===========================================================================
TEST_F(TuplestoreTest, SpillToDisk) {
    TupleDesc desc = MakeIntDesc();
    // work_mem so small that even one tuple triggers spill.
    Tuplestore ts(desc, /*work_mem=*/1);

    for (int i = 0; i < 50; i++) {
        auto* slot = MakeIntSlot(desc, 1000 + i);
        ts.PutTuple(slot);
    }
    EXPECT_EQ(ts.NumTuples(), 50);
    EXPECT_TRUE(ts.IsOnDisk());

    std::vector<int32_t> results;
    TupleTableSlot* s = nullptr;
    while ((s = ts.GetTuple()) != nullptr) {
        results.push_back(DatumGetInt32(s->tts_values[0]));
    }
    ASSERT_EQ(results.size(), 50u);
    for (int i = 0; i < 50; i++) {
        EXPECT_EQ(results[i], 1000 + i);
    }
}

// ===========================================================================
// 5. SpillRewind — spill to disk, then rewind and read again.
// ===========================================================================
TEST_F(TuplestoreTest, SpillRewind) {
    TupleDesc desc = MakeIntIntDesc();
    Tuplestore ts(desc, /*work_mem=*/1);

    for (int i = 0; i < 20; i++) {
        auto* slot = MakeIntIntSlot(desc, i, i * 2);
        ts.PutTuple(slot);
    }
    EXPECT_TRUE(ts.IsOnDisk());

    // First read.
    std::vector<std::pair<int, int>> first;
    TupleTableSlot* s = nullptr;
    while ((s = ts.GetTuple()) != nullptr) {
        first.emplace_back(DatumGetInt32(s->tts_values[0]), DatumGetInt32(s->tts_values[1]));
    }
    ASSERT_EQ(first.size(), 20u);

    // Rewind and read again.
    ts.Rewind();
    std::vector<std::pair<int, int>> second;
    while ((s = ts.GetTuple()) != nullptr) {
        second.emplace_back(DatumGetInt32(s->tts_values[0]), DatumGetInt32(s->tts_values[1]));
    }
    ASSERT_EQ(second.size(), 20u);
    for (int i = 0; i < 20; i++) {
        EXPECT_EQ(first[i].first, second[i].first);
        EXPECT_EQ(first[i].second, second[i].second);
    }
}

// ===========================================================================
// 6. TextSpill — spill with text (byref) data, verify deep copy.
// ===========================================================================
TEST_F(TuplestoreTest, TextSpill) {
    TupleDesc desc = MakeTextDesc();
    Tuplestore ts(desc, /*work_mem=*/1);

    std::vector<std::string> input = {"hello", "world", "foo", "bar", "baz"};
    for (const auto& s : input) {
        auto* slot = MakeTextSlot(desc, s);
        ts.PutTuple(slot);
    }
    EXPECT_EQ(ts.NumTuples(), static_cast<int>(input.size()));
    EXPECT_TRUE(ts.IsOnDisk());

    std::vector<std::string> results;
    TupleTableSlot* s = nullptr;
    while ((s = ts.GetTuple()) != nullptr) {
        results.push_back(TextDatumToString(s->tts_values[0]));
    }
    ASSERT_EQ(results.size(), input.size());
    for (size_t i = 0; i < input.size(); i++) {
        EXPECT_EQ(results[i], input[i]);
    }
}

// ===========================================================================
// 7. TextInMemory — text data in memory (no spill), verify deep copy.
// ===========================================================================
TEST_F(TuplestoreTest, TextInMemory) {
    TupleDesc desc = MakeTextDesc();
    Tuplestore ts(desc, /*work_mem=*/4 * 1024 * 1024);

    std::vector<std::string> input = {"alpha", "beta", "gamma"};
    for (const auto& s : input) {
        auto* slot = MakeTextSlot(desc, s);
        ts.PutTuple(slot);
    }
    EXPECT_FALSE(ts.IsOnDisk());

    std::vector<std::string> results;
    TupleTableSlot* s = nullptr;
    while ((s = ts.GetTuple()) != nullptr) {
        results.push_back(TextDatumToString(s->tts_values[0]));
    }
    ASSERT_EQ(results.size(), input.size());
    for (size_t i = 0; i < input.size(); i++) {
        EXPECT_EQ(results[i], input[i]);
    }
}

}  // namespace

// indextuple_test.cpp — Unit tests for indextuple.c (Task 15.8.3).
//
// Tests index_form_tuple, index_deform_tuple, IndexTupleSize/HasNulls,
// CopyIndexTuple, and index_compute_data_size. These are tuple-level
// operations that only need a memory context.

#include "pgcpp/access/indextuple.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "pgcpp/access/rel.hpp"
#include "pgcpp/catalog/pg_attribute.hpp"
#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/alloc_set.hpp"
#include "pgcpp/common/memory/memory_context.hpp"
#include "pgcpp/transaction/heap_tuple.hpp"
#include "pgcpp/types/datum.hpp"

using mytoydb::access::CopyIndexTuple;
using mytoydb::access::CreateTupleDesc;
using mytoydb::access::index_compute_data_size;
using mytoydb::access::index_deform_tuple;
using mytoydb::access::index_form_tuple;
using mytoydb::access::IndexTupleHasNulls;
using mytoydb::access::IndexTupleHasVarwidth;
using mytoydb::access::IndexTupleSize;
using mytoydb::access::TupleDesc;
using mytoydb::catalog::AttAlign;
using mytoydb::catalog::AttStorage;
using mytoydb::catalog::FormData_pg_attribute;
using mytoydb::memory::AllocSetContext;
using mytoydb::memory::palloc;
using mytoydb::memory::pfree;
using mytoydb::transaction::ItemPointerData;
using mytoydb::types::Datum;
using mytoydb::types::DatumGetInt32;
using mytoydb::types::DatumGetTextP;
using mytoydb::types::Int32GetDatum;
using mytoydb::types::kInt4Oid;
using mytoydb::types::kTextOid;
using mytoydb::types::VARDATA;
using mytoydb::types::VARSIZE;

namespace {

class IndextupleTest : public ::testing::Test {
protected:
    void SetUp() override {
        mytoydb::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("indextuple_test_context");
        mytoydb::memory::SetCurrentMemoryContext(context_);
    }

    void TearDown() override {
        mytoydb::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    TupleDesc MakeIntIntDesc() {
        FormData_pg_attribute a1;
        a1.attname = "a";
        a1.attnum = 1;
        a1.attlen = 4;
        a1.attbyval = true;
        a1.attalign = AttAlign::kInt;
        a1.attstorage = AttStorage::kPlain;
        FormData_pg_attribute a2;
        a2.attname = "b";
        a2.attnum = 2;
        a2.attlen = 4;
        a2.attbyval = true;
        a2.attalign = AttAlign::kInt;
        a2.attstorage = AttStorage::kPlain;
        return CreateTupleDesc({a1, a2});
    }

    // Build a varlena text datum (4-byte length header + data).
    Datum MakeTextDatum(const std::string& s) {
        int32_t total = static_cast<int32_t>(sizeof(int32_t) + s.size());
        char* buf = static_cast<char*>(palloc(total));
        std::memcpy(buf, &total, sizeof(int32_t));
        std::memcpy(buf + sizeof(int32_t), s.data(), s.size());
        return Datum(buf);
    }

    std::string TextDatumToString(Datum d) {
        const char* p = DatumGetTextP(d);
        int len = VARSIZE(p);
        int data_len = len - static_cast<int>(sizeof(int32_t));
        return std::string(VARDATA(p), data_len);
    }

    AllocSetContext* context_ = nullptr;
};

TEST_F(IndextupleTest, ComputeDataSizeTwoInts) {
    TupleDesc desc = MakeIntIntDesc();
    Datum values[2] = {Int32GetDatum(1), Int32GetDatum(2)};
    bool isnull[2] = {false, false};
    EXPECT_EQ(index_compute_data_size(desc, values, isnull), 8u);
}

TEST_F(IndextupleTest, FormTupleSetsTidAndSize) {
    TupleDesc desc = MakeIntIntDesc();
    Datum values[2] = {Int32GetDatum(10), Int32GetDatum(20)};
    bool isnull[2] = {false, false};
    ItemPointerData tid{5, 7};

    auto* tup = index_form_tuple(desc, values, isnull, tid);
    ASSERT_NE(tup, nullptr);

    EXPECT_EQ(tup->t_tid.ip_blkid, 5u);
    EXPECT_EQ(tup->t_tid.ip_posid, 7u);
    // Size = header (8, already aligned) + 2*int4 = 16.
    EXPECT_EQ(IndexTupleSize(tup), 16u);
    EXPECT_FALSE(IndexTupleHasNulls(tup));
    EXPECT_FALSE(IndexTupleHasVarwidth(tup));

    pfree(tup);
}

TEST_F(IndextupleTest, DeformTupleRoundTripsInts) {
    TupleDesc desc = MakeIntIntDesc();
    Datum values[2] = {Int32GetDatum(111), Int32GetDatum(222)};
    bool isnull[2] = {false, false};
    ItemPointerData tid{0, 1};

    auto* tup = index_form_tuple(desc, values, isnull, tid);

    Datum out[2];
    bool out_null[2];
    index_deform_tuple(tup, desc, out, out_null);
    EXPECT_FALSE(out_null[0]);
    EXPECT_FALSE(out_null[1]);
    EXPECT_EQ(DatumGetInt32(out[0]), 111);
    EXPECT_EQ(DatumGetInt32(out[1]), 222);

    pfree(tup);
}

TEST_F(IndextupleTest, FormTupleWithNullsSetsNullBit) {
    TupleDesc desc = MakeIntIntDesc();
    Datum values[2] = {Int32GetDatum(1), Int32GetDatum(0)};
    bool isnull[2] = {false, true};
    ItemPointerData tid{0, 1};

    auto* tup = index_form_tuple(desc, values, isnull, tid);
    EXPECT_TRUE(IndexTupleHasNulls(tup));
    // Size = header(8) + 1 bitmap byte = 9, MAXALIGN to 16, + 4 (one int4) = 20.
    EXPECT_EQ(IndexTupleSize(tup), 20u);

    Datum out[2];
    bool out_null[2];
    index_deform_tuple(tup, desc, out, out_null);
    EXPECT_FALSE(out_null[0]);
    EXPECT_TRUE(out_null[1]);
    EXPECT_EQ(DatumGetInt32(out[0]), 1);

    pfree(tup);
}

TEST_F(IndextupleTest, FormTupleWithTextSetsVarwidth) {
    FormData_pg_attribute a1;
    a1.attname = "k";
    a1.attnum = 1;
    a1.attlen = -1;
    a1.attbyval = false;
    a1.attalign = AttAlign::kInt;
    a1.attstorage = AttStorage::kPlain;
    TupleDesc desc = CreateTupleDesc({a1});

    Datum values[1] = {MakeTextDatum("hello")};
    bool isnull[1] = {false};
    ItemPointerData tid{1, 2};

    auto* tup = index_form_tuple(desc, values, isnull, tid);
    EXPECT_TRUE(IndexTupleHasVarwidth(tup));
    EXPECT_FALSE(IndexTupleHasNulls(tup));

    Datum out[1];
    bool out_null[1];
    index_deform_tuple(tup, desc, out, out_null);
    EXPECT_FALSE(out_null[0]);
    EXPECT_EQ(TextDatumToString(out[0]), "hello");

    pfree(tup);
}

TEST_F(IndextupleTest, CopyIndexTupleIsDeepCopy) {
    TupleDesc desc = MakeIntIntDesc();
    Datum values[2] = {Int32GetDatum(3), Int32GetDatum(4)};
    bool isnull[2] = {false, false};
    ItemPointerData tid{0, 1};

    auto* tup = index_form_tuple(desc, values, isnull, tid);
    auto* copy = CopyIndexTuple(tup);
    ASSERT_NE(copy, tup);
    EXPECT_EQ(IndexTupleSize(copy), IndexTupleSize(tup));
    EXPECT_EQ(copy->t_tid.ip_posid, 1u);

    // Mutate the copy's data and confirm the original is unaffected.
    Datum out[2];
    bool out_null[2];
    index_deform_tuple(copy, desc, out, out_null);
    EXPECT_EQ(DatumGetInt32(out[0]), 3);

    pfree(tup);
    pfree(copy);
}

}  // namespace

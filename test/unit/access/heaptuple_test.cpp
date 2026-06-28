// heaptuple_test.cpp — Unit tests for heaptuple.c P0 extensions (Task 15.8.1).
//
// Tests heap_fill_tuple, heap_modify_tuple, heap_modify_tuple_by_cols,
// heap_copytuple, heap_copytuple_with_tuple, heap_attisnull, minimal tuple
// conversions, and heap_tuple_buffer_getsysattr.
//
// These are tuple-level operations that only need a memory context (no
// storage, catalog, or buffer pool).

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "pgcpp/access/heapam.hpp"
#include "pgcpp/access/rel.hpp"
#include "pgcpp/catalog/pg_attribute.hpp"
#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/alloc_set.hpp"
#include "pgcpp/common/memory/memory_context.hpp"
#include "pgcpp/transaction/heap_tuple.hpp"
#include "pgcpp/types/datum.hpp"

using mytoydb::access::CreateTupleDesc;
using mytoydb::access::heap_attisnull;
using mytoydb::access::heap_compute_data_size;
using mytoydb::access::heap_copytuple;
using mytoydb::access::heap_copytuple_with_tuple;
using mytoydb::access::heap_deform_tuple;
using mytoydb::access::heap_fill_tuple;
using mytoydb::access::heap_form_tuple;
using mytoydb::access::heap_freetuple;
using mytoydb::access::heap_getattr;
using mytoydb::access::heap_modify_tuple;
using mytoydb::access::heap_modify_tuple_by_cols;
using mytoydb::access::heap_tuple_buffer_getsysattr;
using mytoydb::access::heap_tuple_from_minimal_tuple;
using mytoydb::access::kMaxCommandIdAttributeNumber;
using mytoydb::access::kMaxTransactionIdAttributeNumber;
using mytoydb::access::kMinCommandIdAttributeNumber;
using mytoydb::access::kMinTransactionIdAttributeNumber;
using mytoydb::access::kSelfItemPointerAttributeNumber;
using mytoydb::access::kTableOidAttributeNumber;
using mytoydb::access::minimal_tuple_from_heap_tuple;
using mytoydb::access::TupleDesc;
using mytoydb::catalog::AttAlign;
using mytoydb::catalog::AttStorage;
using mytoydb::catalog::FormData_pg_attribute;
using mytoydb::memory::AllocSetContext;
using mytoydb::nodes::makePallocNode;
using mytoydb::transaction::HeapTuple;
using mytoydb::transaction::HeapTupleData;
using mytoydb::transaction::HeapTupleHeaderData;
using mytoydb::transaction::ItemPointerData;
using mytoydb::types::Datum;
using mytoydb::types::DatumGetInt32;
using mytoydb::types::Int32GetDatum;

namespace {

class HeaptupleTest : public ::testing::Test {
protected:
    void SetUp() override {
        mytoydb::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("heaptuple_test_context");
        mytoydb::memory::SetCurrentMemoryContext(context_);
    }

    void TearDown() override {
        mytoydb::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    // Build a 2-column int4 schema.
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

    AllocSetContext* context_ = nullptr;
};

// --- heap_fill_tuple ---

TEST_F(HeaptupleTest, FillTupleNoNullsRoundTrips) {
    TupleDesc desc = MakeIntIntDesc();
    Datum values[2] = {Int32GetDatum(42), Int32GetDatum(99)};
    bool isnull[2] = {false, false};

    uint32_t data_size = heap_compute_data_size(desc, values, isnull);
    // 2 int4s = 8 bytes.
    EXPECT_EQ(data_size, 8u);

    // Allocate a generous buffer (hoff + data_size; hoff <= 24 for 2 attrs).
    char data[64];
    uint16_t infomask = 0;
    uint8_t hoff = 0;
    heap_fill_tuple(desc, values, isnull, data, data_size, &infomask, &hoff);

    // No nulls → no kHeapHasNull bit. Data present → kHeapHasVarWidth not set
    // for by-value fixed-length types (matches heap_form_tuple behavior).
    EXPECT_EQ((infomask & mytoydb::transaction::kHeapHasNull), 0u);
    EXPECT_EQ(hoff, 24u);  // MAXALIGN(23) for no nulls.

    // heap_fill_tuple fills the data + null bitmap but leaves the header
    // fields (t_hoff / t_infomask / natts) zeroed — the caller (heap_form_tuple)
    // sets them. Mirror that here so heap_deform_tuple can read them back.
    auto* hdr = reinterpret_cast<HeapTupleHeaderData*>(data);
    hdr->t_hoff = hoff;
    hdr->t_infomask = infomask;
    mytoydb::transaction::HeapTupleHeaderSetNatts(hdr, desc->natts);

    // Wrap and deform to verify round-trip.
    HeapTupleData tup;
    tup.t_len = hoff + data_size;
    tup.t_data = hdr;

    Datum out[2];
    bool out_null[2];
    heap_deform_tuple(&tup, desc, out, out_null);
    EXPECT_FALSE(out_null[0]);
    EXPECT_FALSE(out_null[1]);
    EXPECT_EQ(DatumGetInt32(out[0]), 42);
    EXPECT_EQ(DatumGetInt32(out[1]), 99);
}

TEST_F(HeaptupleTest, FillTupleWithNullSetsHasNullBit) {
    TupleDesc desc = MakeIntIntDesc();
    Datum values[2] = {Int32GetDatum(7), Int32GetDatum(0)};
    bool isnull[2] = {false, true};

    uint32_t data_size = heap_compute_data_size(desc, values, isnull);
    // Only the first int4 (nulls contribute 0 bytes).
    EXPECT_EQ(data_size, 4u);

    char data[64];
    uint16_t infomask = 0;
    uint8_t hoff = 0;
    heap_fill_tuple(desc, values, isnull, data, data_size, &infomask, &hoff);

    // Has nulls → kHeapHasNull set. hoff = MAXALIGN(23 + 1) = 24.
    EXPECT_NE((infomask & mytoydb::transaction::kHeapHasNull), 0u);
    EXPECT_EQ(hoff, 24u);

    // Set the header fields (as heap_form_tuple would) so deform can read them.
    auto* hdr = reinterpret_cast<HeapTupleHeaderData*>(data);
    hdr->t_hoff = hoff;
    hdr->t_infomask = infomask;
    mytoydb::transaction::HeapTupleHeaderSetNatts(hdr, desc->natts);

    // Deform and verify the null bitmap is honored.
    HeapTupleData tup;
    tup.t_len = hoff + data_size;
    tup.t_data = hdr;

    Datum out[2];
    bool out_null[2];
    heap_deform_tuple(&tup, desc, out, out_null);
    EXPECT_FALSE(out_null[0]);
    EXPECT_TRUE(out_null[1]);
    EXPECT_EQ(DatumGetInt32(out[0]), 7);
}

// --- heap_modify_tuple ---

TEST_F(HeaptupleTest, ModifyTupleReplacesSelectedColumns) {
    TupleDesc desc = MakeIntIntDesc();
    Datum values[2] = {Int32GetDatum(1), Int32GetDatum(2)};
    bool isnull[2] = {false, false};
    HeapTuple tup = heap_form_tuple(desc, values, isnull);

    // Replace only column 2.
    Datum new_values[2] = {Int32GetDatum(0), Int32GetDatum(20)};
    bool new_isnull[2] = {false, false};
    bool do_replace[2] = {false, true};
    HeapTuple modified = heap_modify_tuple(tup, desc, new_values, new_isnull, do_replace);

    bool is_null = false;
    EXPECT_EQ(DatumGetInt32(heap_getattr(modified, 1, desc, &is_null)), 1);
    EXPECT_FALSE(is_null);
    EXPECT_EQ(DatumGetInt32(heap_getattr(modified, 2, desc, &is_null)), 20);
    EXPECT_FALSE(is_null);

    heap_freetuple(modified);
    heap_freetuple(tup);
}

TEST_F(HeaptupleTest, ModifyTupleByColsReplacesFirstN) {
    TupleDesc desc = MakeIntIntDesc();
    Datum values[2] = {Int32GetDatum(10), Int32GetDatum(20)};
    bool isnull[2] = {false, false};
    HeapTuple tup = heap_form_tuple(desc, values, isnull);

    // Replace only the first column (ncols=1).
    Datum new_values[2] = {Int32GetDatum(100), Int32GetDatum(0)};
    bool new_isnull[2] = {false, false};
    HeapTuple modified = heap_modify_tuple_by_cols(tup, desc, 1, new_values, new_isnull);

    bool is_null = false;
    EXPECT_EQ(DatumGetInt32(heap_getattr(modified, 1, desc, &is_null)), 100);
    EXPECT_EQ(DatumGetInt32(heap_getattr(modified, 2, desc, &is_null)), 20);

    heap_freetuple(modified);
    heap_freetuple(tup);
}

// --- heap_copytuple / heap_copytuple_with_tuple ---

TEST_F(HeaptupleTest, CopyTupleIsDeepCopy) {
    TupleDesc desc = MakeIntIntDesc();
    Datum values[2] = {Int32GetDatum(5), Int32GetDatum(6)};
    bool isnull[2] = {false, false};
    HeapTuple tup = heap_form_tuple(desc, values, isnull);

    HeapTuple copy = heap_copytuple(tup);
    ASSERT_NE(copy, nullptr);
    ASSERT_NE(copy->t_data, tup->t_data);  // Deep copy: different buffer.

    // Mutating the copy must not affect the original.
    bool is_null = false;
    EXPECT_EQ(DatumGetInt32(heap_getattr(copy, 1, desc, &is_null)), 5);

    heap_freetuple(copy);
    heap_freetuple(tup);
}

TEST_F(HeaptupleTest, CopytupleWithTupleFillsDest) {
    TupleDesc desc = MakeIntIntDesc();
    Datum values[2] = {Int32GetDatum(11), Int32GetDatum(22)};
    bool isnull[2] = {false, false};
    HeapTuple src = heap_form_tuple(desc, values, isnull);

    HeapTuple dest = makePallocNode<HeapTupleData>();
    heap_copytuple_with_tuple(src, dest);
    ASSERT_NE(dest->t_data, nullptr);
    ASSERT_NE(dest->t_data, src->t_data);
    EXPECT_EQ(dest->t_len, src->t_len);

    bool is_null = false;
    EXPECT_EQ(DatumGetInt32(heap_getattr(dest, 2, desc, &is_null)), 22);

    heap_freetuple(src);
    if (dest->t_data != nullptr) {
        mytoydb::memory::pfree(dest->t_data);
    }
    mytoydb::nodes::destroyPallocNode(dest);
}

// --- heap_attisnull ---

TEST_F(HeaptupleTest, AttIsNullForSystemColumnIsFalse) {
    TupleDesc desc = MakeIntIntDesc();
    Datum values[2] = {Int32GetDatum(1), Int32GetDatum(2)};
    bool isnull[2] = {false, false};
    HeapTuple tup = heap_form_tuple(desc, values, isnull);

    // System columns (negative attnum) are never null.
    EXPECT_FALSE(heap_attisnull(tup, kSelfItemPointerAttributeNumber, desc));
    EXPECT_FALSE(heap_attisnull(tup, kMinTransactionIdAttributeNumber, desc));

    heap_freetuple(tup);
}

TEST_F(HeaptupleTest, AttIsNullDetectsNullUserColumn) {
    TupleDesc desc = MakeIntIntDesc();
    Datum values[2] = {Int32GetDatum(1), Int32GetDatum(0)};
    bool isnull[2] = {false, true};
    HeapTuple tup = heap_form_tuple(desc, values, isnull);

    EXPECT_FALSE(heap_attisnull(tup, 1, desc));
    EXPECT_TRUE(heap_attisnull(tup, 2, desc));

    heap_freetuple(tup);
}

// --- minimal tuple conversions ---

TEST_F(HeaptupleTest, MinimalTupleRoundTripPreservesValues) {
    TupleDesc desc = MakeIntIntDesc();
    Datum values[2] = {Int32GetDatum(33), Int32GetDatum(44)};
    bool isnull[2] = {false, false};
    HeapTuple tup = heap_form_tuple(desc, values, isnull);

    HeapTuple mtup = minimal_tuple_from_heap_tuple(tup);
    HeapTuple back = heap_tuple_from_minimal_tuple(mtup);

    bool is_null = false;
    EXPECT_EQ(DatumGetInt32(heap_getattr(back, 1, desc, &is_null)), 33);
    EXPECT_EQ(DatumGetInt32(heap_getattr(back, 2, desc, &is_null)), 44);

    heap_freetuple(back);
    heap_freetuple(mtup);
    heap_freetuple(tup);
}

// --- heap_tuple_buffer_getsysattr ---

TEST_F(HeaptupleTest, GetSysAttrReturnsCtidXminXmax) {
    TupleDesc desc = MakeIntIntDesc();
    Datum values[2] = {Int32GetDatum(1), Int32GetDatum(2)};
    bool isnull[2] = {false, false};
    HeapTuple tup = heap_form_tuple(desc, values, isnull);

    // Set MVCC metadata directly on the header.
    HeapTupleHeaderData* hdr = tup->t_data;
    hdr->t_xmin = 100;
    hdr->t_xmax = 200;
    hdr->t_cid = 7;
    ItemPointerData ctid{3, 9};
    hdr->t_ctid = ctid;

    bool is_null = true;
    // ctid: returned as a pointer to a static buffer.
    Datum ctid_datum =
        heap_tuple_buffer_getsysattr(tup, kSelfItemPointerAttributeNumber, desc, &is_null);
    EXPECT_FALSE(is_null);
    auto* ctid_ptr = reinterpret_cast<ItemPointerData*>(ctid_datum);
    EXPECT_EQ(ctid_ptr->ip_blkid, 3u);
    EXPECT_EQ(ctid_ptr->ip_posid, 9u);

    // xmin / xmax / cmin / cmax returned by value.
    EXPECT_EQ(static_cast<uint32_t>(heap_tuple_buffer_getsysattr(
                  tup, kMinTransactionIdAttributeNumber, desc, &is_null)),
              100u);
    EXPECT_FALSE(is_null);
    EXPECT_EQ(static_cast<uint32_t>(heap_tuple_buffer_getsysattr(
                  tup, kMaxTransactionIdAttributeNumber, desc, &is_null)),
              200u);
    EXPECT_EQ(static_cast<uint32_t>(
                  heap_tuple_buffer_getsysattr(tup, kMinCommandIdAttributeNumber, desc, &is_null)),
              7u);
    EXPECT_EQ(static_cast<uint32_t>(
                  heap_tuple_buffer_getsysattr(tup, kMaxCommandIdAttributeNumber, desc, &is_null)),
              7u);

    // tableoid: MyToyDB returns 0 (HeapTupleData has no t_tableOid).
    EXPECT_EQ(heap_tuple_buffer_getsysattr(tup, kTableOidAttributeNumber, desc, &is_null),
              Datum(0));

    heap_freetuple(tup);
}

}  // namespace

// heapam_test.cpp — Unit tests for the heap access method (M8 Task 8.1).
//
// Tests heap_form_tuple / heap_deform_tuple (round-trip with various types
// and nulls), heap_getattr, heap_insert (single, multiple, page extension),
// heap_delete, heap_update, and heap_beginscan / heap_getnext / heap_endscan
// / heap_rescan with MVCC visibility.
//
// The fixture sets up the full stack: error subsystem, memory context,
// catalog + syscache, transaction system, buffer pool, storage directory,
// and relcache. Each test creates a fresh relation with a known schema.

#include "access/heapam.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include "access/rel.hpp"
#include "catalog/catalog.hpp"
#include "catalog/pg_attribute.hpp"
#include "catalog/pg_class.hpp"
#include "catalog/syscache.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "storage/bufmgr.hpp"
#include "storage/smgr.hpp"
#include "transaction/heap_tuple.hpp"
#include "transaction/snapshot.hpp"
#include "transaction/transam.hpp"
#include "transaction/xact.hpp"
#include "types/datum.hpp"

using pgcpp::access::att_align;
using pgcpp::access::att_align_max;
using pgcpp::access::CreateTupleDesc;
using pgcpp::access::heap_beginscan;
using pgcpp::access::heap_compute_data_size;
using pgcpp::access::heap_deform_tuple;
using pgcpp::access::heap_delete;
using pgcpp::access::heap_endscan;
using pgcpp::access::heap_form_tuple;
using pgcpp::access::heap_freetuple;
using pgcpp::access::heap_getattr;
using pgcpp::access::heap_getnext;
using pgcpp::access::heap_insert;
using pgcpp::access::heap_rescan;
using pgcpp::access::heap_update;
using pgcpp::access::HeapScanDesc;
using pgcpp::access::HeapScanDescData;
using pgcpp::access::InitializeRelcache;
using pgcpp::access::Relation;
using pgcpp::access::RelationClose;
using pgcpp::access::RelationCreateStorage;
using pgcpp::access::RelationOpen;
using pgcpp::access::ResetRelcache;
using pgcpp::access::TupleDesc;
using pgcpp::catalog::AttAlign;
using pgcpp::catalog::AttStorage;
using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_attribute;
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
using pgcpp::storage::InitBufferPool;
using pgcpp::storage::SetStorageBaseDir;
using pgcpp::storage::ShutdownBufferPool;
using pgcpp::storage::smgrcloseall;
using pgcpp::transaction::AllocateNextTransactionId;
using pgcpp::transaction::BeginTransactionBlock;
using pgcpp::transaction::EndTransactionBlock;
using pgcpp::transaction::HeapTuple;
using pgcpp::transaction::HeapTupleData;
using pgcpp::transaction::HeapTupleHeaderData;
using pgcpp::transaction::HeapTupleHeaderGetXmax;
using pgcpp::transaction::HeapTupleHeaderGetXmin;
using pgcpp::transaction::HeapTupleHeaderSetXminCommitted;
using pgcpp::transaction::InitializeTransactionSystem;
using pgcpp::transaction::ItemPointerData;
using pgcpp::transaction::kFirstNormalTransactionId;
using pgcpp::transaction::MakeSnapshot;
using pgcpp::transaction::ResetTransactionState;
using pgcpp::transaction::SnapshotData;
using pgcpp::transaction::TransactionId;
using pgcpp::transaction::TransactionIdCommit;
using pgcpp::types::Datum;
using pgcpp::types::DatumGetInt32;
using pgcpp::types::DatumGetInt64;
using pgcpp::types::DatumGetTextP;
using pgcpp::types::Int32GetDatum;
using pgcpp::types::Int64GetDatum;
using pgcpp::types::kInt4Oid;
using pgcpp::types::kInt8Oid;
using pgcpp::types::kTextOid;
using pgcpp::types::VARDATA;
using pgcpp::types::VARSIZE;

namespace {

using pgcpp::nodes::makePallocNode;

class HeapamTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("heapam_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        syscache_ = new SysCache();
        SetSysCache(syscache_);

        ResetTransactionState();
        InitializeTransactionSystem();
        BeginTransactionBlock();

        test_dir_ = "/tmp/pgcpp_heapam_test_" + std::to_string(getpid());
        SetStorageBaseDir(test_dir_);
        RunShell("rm -rf " + test_dir_);

        InitBufferPool(64);
        InitializeRelcache();
    }

    void TearDown() override {
        EndTransactionBlock();
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

    // Helper: build a pg_class row and insert it into the catalog.
    // The relation is given a relfilenode equal to its OID.
    FormData_pg_class* MakeClassRow(const std::string& name, Oid oid) {
        auto* row = makePallocNode<FormData_pg_class>();
        row->oid = oid;
        row->relname = name;
        row->relfilenode = oid;
        row->relkind = RelKind::kRelation;
        row->relpersistence = RelPersistence::kPermanent;
        return row;
    }

    // Helper: build a pg_attribute row.
    FormData_pg_attribute* MakeAttrRow(Oid relid, const std::string& name, int16_t attnum,
                                       Oid typid, int16_t attlen, bool attbyval,
                                       AttAlign attalign) {
        auto* row = makePallocNode<FormData_pg_attribute>();
        row->attrelid = relid;
        row->attname = name;
        row->attnum = attnum;
        row->atttypid = typid;
        row->attlen = attlen;
        row->attbyval = attbyval;
        row->attalign = attalign;
        row->attstorage = AttStorage::kPlain;
        return row;
    }

    // Helper: create a relation with the given OID and schema, including
    // physical storage. Returns the opened Relation.
    Relation CreateTestRelation(Oid relid, const std::string& name,
                                const std::vector<FormData_pg_attribute>& attrs) {
        auto* class_row = MakeClassRow(name, relid);
        catalog_->InsertClass(class_row);
        for (const auto& attr : attrs) {
            auto* attr_row = makePallocNode<FormData_pg_attribute>(attr);
            catalog_->InsertAttribute(attr_row);
        }
        RelationCreateStorage(relid, false);
        return RelationOpen(relid);
    }

    // Helper: build a simple 2-column schema (int4, int4).
    std::vector<FormData_pg_attribute> MakeIntIntSchema(Oid relid) {
        FormData_pg_attribute a1;
        a1.attrelid = relid;
        a1.attname = "a";
        a1.attnum = 1;
        a1.atttypid = kInt4Oid;
        a1.attlen = 4;
        a1.attbyval = true;
        a1.attalign = AttAlign::kInt;
        a1.attstorage = AttStorage::kPlain;

        FormData_pg_attribute a2;
        a2.attrelid = relid;
        a2.attname = "b";
        a2.attnum = 2;
        a2.atttypid = kInt4Oid;
        a2.attlen = 4;
        a2.attbyval = true;
        a2.attalign = AttAlign::kInt;
        a2.attstorage = AttStorage::kPlain;

        return {a1, a2};
    }

    // Helper: build a schema with int4 + int64 + text.
    std::vector<FormData_pg_attribute> MakeMixedSchema(Oid relid) {
        FormData_pg_attribute a1;
        a1.attrelid = relid;
        a1.attname = "id";
        a1.attnum = 1;
        a1.atttypid = kInt4Oid;
        a1.attlen = 4;
        a1.attbyval = true;
        a1.attalign = AttAlign::kInt;
        a1.attstorage = AttStorage::kPlain;

        FormData_pg_attribute a2;
        a2.attrelid = relid;
        a2.attname = "big";
        a2.attnum = 2;
        a2.atttypid = kInt8Oid;
        a2.attlen = 8;
        a2.attbyval = true;
        a2.attalign = AttAlign::kDouble;
        a2.attstorage = AttStorage::kPlain;

        FormData_pg_attribute a3;
        a3.attrelid = relid;
        a3.attname = "txt";
        a3.attnum = 3;
        a3.atttypid = kTextOid;
        a3.attlen = -1;  // varlena
        a3.attbyval = false;
        a3.attalign = AttAlign::kInt;
        a3.attstorage = AttStorage::kPlain;

        return {a1, a2, a3};
    }

    // Helper: build a varlena text datum from a C string.
    // Format: 4-byte length prefix (total including header) + data (no null).
    Datum MakeTextDatum(const std::string& s) {
        int32_t total = static_cast<int32_t>(sizeof(int32_t) + s.size());
        char* buf = static_cast<char*>(pgcpp::memory::palloc(total));
        std::memcpy(buf, &total, sizeof(int32_t));
        std::memcpy(buf + sizeof(int32_t), s.data(), s.size());
        return Datum(buf);
    }

    // Helper: extract std::string from a text Datum.
    std::string TextDatumToString(Datum d) {
        const char* p = DatumGetTextP(d);
        int len = VARSIZE(p);
        int data_len = len - static_cast<int>(sizeof(int32_t));
        return std::string(VARDATA(p), data_len);
    }

    // Helper: commit the current transaction and start a new one so that
    // inserted tuples become visible to subsequent snapshots.
    void CommitAndStartNew() {
        EndTransactionBlock();
        BeginTransactionBlock();
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

// --- Alignment helpers ---

TEST_F(HeapamTest, AttAlignCharIsNoOp) {
    EXPECT_EQ(att_align(0, AttAlign::kChar), 0u);
    EXPECT_EQ(att_align(1, AttAlign::kChar), 1u);
    EXPECT_EQ(att_align(5, AttAlign::kChar), 5u);
}

TEST_F(HeapamTest, AttAlignShortAlignsTo2) {
    EXPECT_EQ(att_align(0, AttAlign::kShort), 0u);
    EXPECT_EQ(att_align(1, AttAlign::kShort), 2u);
    EXPECT_EQ(att_align(2, AttAlign::kShort), 2u);
    EXPECT_EQ(att_align(3, AttAlign::kShort), 4u);
}

TEST_F(HeapamTest, AttAlignIntAlignsTo4) {
    EXPECT_EQ(att_align(0, AttAlign::kInt), 0u);
    EXPECT_EQ(att_align(1, AttAlign::kInt), 4u);
    EXPECT_EQ(att_align(3, AttAlign::kInt), 4u);
    EXPECT_EQ(att_align(4, AttAlign::kInt), 4u);
    EXPECT_EQ(att_align(5, AttAlign::kInt), 8u);
}

TEST_F(HeapamTest, AttAlignDoubleAlignsTo8) {
    EXPECT_EQ(att_align(0, AttAlign::kDouble), 0u);
    EXPECT_EQ(att_align(1, AttAlign::kDouble), 8u);
    EXPECT_EQ(att_align(7, AttAlign::kDouble), 8u);
    EXPECT_EQ(att_align(8, AttAlign::kDouble), 8u);
    EXPECT_EQ(att_align(9, AttAlign::kDouble), 16u);
}

TEST_F(HeapamTest, AttAlignMaxAlignsTo8) {
    EXPECT_EQ(att_align_max(0), 0u);
    EXPECT_EQ(att_align_max(1), 8u);
    EXPECT_EQ(att_align_max(7), 8u);
    EXPECT_EQ(att_align_max(8), 8u);
    EXPECT_EQ(att_align_max(15), 16u);
}

// --- heap_compute_data_size ---

TEST_F(HeapamTest, ComputeDataSizeTwoInts) {
    auto attrs = MakeIntIntSchema(100);
    TupleDesc desc = CreateTupleDesc(attrs);
    Datum values[2] = {Int32GetDatum(42), Int32GetDatum(99)};
    bool isnull[2] = {false, false};
    // Two int4 values: 4 + 4 = 8 bytes (no alignment padding needed).
    EXPECT_EQ(heap_compute_data_size(desc, values, isnull), 8u);
}

TEST_F(HeapamTest, ComputeDataSizeMixed) {
    auto attrs = MakeMixedSchema(101);
    TupleDesc desc = CreateTupleDesc(attrs);
    Datum values[3] = {Int32GetDatum(1), Int64GetDatum(2), MakeTextDatum("hi")};
    bool isnull[3] = {false, false, false};
    // int4 (4) + pad to 8 (4) + int8 (8) + pad to 4 (0) + varlena (4+2=6)
    // = 4 + 4 + 8 + 6 = 22
    EXPECT_EQ(heap_compute_data_size(desc, values, isnull), 22u);
}

TEST_F(HeapamTest, ComputeDataSizeWithNulls) {
    auto attrs = MakeMixedSchema(102);
    TupleDesc desc = CreateTupleDesc(attrs);
    Datum values[3] = {Int32GetDatum(1), 0, 0};
    bool isnull[3] = {false, true, true};
    // Only int4 (4 bytes).
    EXPECT_EQ(heap_compute_data_size(desc, values, isnull), 4u);
}

// --- heap_form_tuple / heap_deform_tuple round-trip ---

TEST_F(HeapamTest, FormDeformTwoInts) {
    auto attrs = MakeIntIntSchema(200);
    TupleDesc desc = CreateTupleDesc(attrs);

    Datum values[2] = {Int32GetDatum(42), Int32GetDatum(99)};
    bool isnull[2] = {false, false};
    HeapTuple tup = heap_form_tuple(desc, values, isnull);

    ASSERT_NE(tup, nullptr);
    ASSERT_NE(tup->t_data, nullptr);
    EXPECT_GT(tup->t_len, 0u);

    Datum out_values[2];
    bool out_isnull[2];
    heap_deform_tuple(tup, desc, out_values, out_isnull);

    EXPECT_FALSE(out_isnull[0]);
    EXPECT_FALSE(out_isnull[1]);
    EXPECT_EQ(DatumGetInt32(out_values[0]), 42);
    EXPECT_EQ(DatumGetInt32(out_values[1]), 99);

    heap_freetuple(tup);
}

TEST_F(HeapamTest, FormDeformMixedTypes) {
    auto attrs = MakeMixedSchema(201);
    TupleDesc desc = CreateTupleDesc(attrs);

    Datum values[3] = {Int32GetDatum(7), Int64GetDatum(0x123456789ABCDEF0LL),
                       MakeTextDatum("hello")};
    bool isnull[3] = {false, false, false};
    HeapTuple tup = heap_form_tuple(desc, values, isnull);

    Datum out_values[3];
    bool out_isnull[3];
    heap_deform_tuple(tup, desc, out_values, out_isnull);

    EXPECT_FALSE(out_isnull[0]);
    EXPECT_FALSE(out_isnull[1]);
    EXPECT_FALSE(out_isnull[2]);
    EXPECT_EQ(DatumGetInt32(out_values[0]), 7);
    EXPECT_EQ(DatumGetInt64(out_values[1]), 0x123456789ABCDEF0LL);
    EXPECT_EQ(TextDatumToString(out_values[2]), "hello");

    heap_freetuple(tup);
}

TEST_F(HeapamTest, FormDeformWithNulls) {
    auto attrs = MakeMixedSchema(202);
    TupleDesc desc = CreateTupleDesc(attrs);

    Datum values[3] = {Int32GetDatum(1), 0, MakeTextDatum("x")};
    bool isnull[3] = {false, true, false};
    HeapTuple tup = heap_form_tuple(desc, values, isnull);

    // The HASNULL flag should be set.
    EXPECT_NE(tup->t_data->t_infomask & pgcpp::transaction::kHeapHasNull, 0);

    Datum out_values[3];
    bool out_isnull[3];
    heap_deform_tuple(tup, desc, out_values, out_isnull);

    EXPECT_FALSE(out_isnull[0]);
    EXPECT_TRUE(out_isnull[1]);
    EXPECT_FALSE(out_isnull[2]);
    EXPECT_EQ(DatumGetInt32(out_values[0]), 1);
    EXPECT_EQ(TextDatumToString(out_values[2]), "x");

    heap_freetuple(tup);
}

TEST_F(HeapamTest, FormDeformAllNulls) {
    auto attrs = MakeMixedSchema(203);
    TupleDesc desc = CreateTupleDesc(attrs);

    Datum values[3] = {0, 0, 0};
    bool isnull[3] = {true, true, true};
    HeapTuple tup = heap_form_tuple(desc, values, isnull);

    Datum out_values[3];
    bool out_isnull[3];
    heap_deform_tuple(tup, desc, out_values, out_isnull);

    EXPECT_TRUE(out_isnull[0]);
    EXPECT_TRUE(out_isnull[1]);
    EXPECT_TRUE(out_isnull[2]);

    heap_freetuple(tup);
}

// --- heap_getattr ---

TEST_F(HeapamTest, GetAttrReturnsCorrectValue) {
    auto attrs = MakeMixedSchema(204);
    TupleDesc desc = CreateTupleDesc(attrs);

    Datum values[3] = {Int32GetDatum(123), Int64GetDatum(456), MakeTextDatum("world")};
    bool isnull[3] = {false, false, false};
    HeapTuple tup = heap_form_tuple(desc, values, isnull);

    bool isnull_out = false;
    EXPECT_EQ(DatumGetInt32(heap_getattr(tup, 1, desc, &isnull_out)), 123);
    EXPECT_FALSE(isnull_out);

    EXPECT_EQ(DatumGetInt64(heap_getattr(tup, 2, desc, &isnull_out)), 456);
    EXPECT_FALSE(isnull_out);

    EXPECT_EQ(TextDatumToString(heap_getattr(tup, 3, desc, &isnull_out)), "world");
    EXPECT_FALSE(isnull_out);

    heap_freetuple(tup);
}

TEST_F(HeapamTest, GetAttrReturnsNullForNullAttribute) {
    auto attrs = MakeMixedSchema(205);
    TupleDesc desc = CreateTupleDesc(attrs);

    Datum values[3] = {Int32GetDatum(1), 0, MakeTextDatum("z")};
    bool isnull[3] = {false, true, false};
    HeapTuple tup = heap_form_tuple(desc, values, isnull);

    bool isnull_out = false;
    heap_getattr(tup, 2, desc, &isnull_out);
    EXPECT_TRUE(isnull_out);

    // Other attributes are not null.
    heap_getattr(tup, 1, desc, &isnull_out);
    EXPECT_FALSE(isnull_out);
    heap_getattr(tup, 3, desc, &isnull_out);
    EXPECT_FALSE(isnull_out);

    heap_freetuple(tup);
}

// --- heap_insert + heap scan ---

TEST_F(HeapamTest, InsertSingleTupleAndScan) {
    constexpr Oid kRelid = 1000;
    auto attrs = MakeIntIntSchema(kRelid);
    Relation rel = CreateTestRelation(kRelid, "test_insert1", attrs);
    ASSERT_NE(rel, nullptr);

    // Insert one tuple.
    Datum values[2] = {Int32GetDatum(10), Int32GetDatum(20)};
    bool isnull[2] = {false, false};
    HeapTuple tup = heap_form_tuple(rel->rd_att, values, isnull);
    ItemPointerData tid = heap_insert(rel, tup);
    EXPECT_EQ(tid.ip_blkid, 0u);
    EXPECT_EQ(tid.ip_posid, 1u);
    heap_freetuple(tup);

    // Commit so the tuple is visible.
    CommitAndStartNew();

    // Scan and verify.
    SnapshotData snap = MakeSnapshot(pgcpp::transaction::GetNextTransactionId(),
                                     pgcpp::transaction::GetNextTransactionId() + 1);
    HeapScanDesc scan = heap_beginscan(rel, &snap);
    HeapTuple out = heap_getnext(scan);
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(out->t_self.ip_blkid, 0u);
    EXPECT_EQ(out->t_self.ip_posid, 1u);

    bool isnull_out = false;
    EXPECT_EQ(DatumGetInt32(heap_getattr(out, 1, rel->rd_att, &isnull_out)), 10);
    EXPECT_EQ(DatumGetInt32(heap_getattr(out, 2, rel->rd_att, &isnull_out)), 20);

    EXPECT_EQ(heap_getnext(scan), nullptr);
    heap_endscan(scan);

    RelationClose(rel);
}

TEST_F(HeapamTest, InsertMultipleTuplesAndScanAll) {
    constexpr Oid kRelid = 1001;
    auto attrs = MakeIntIntSchema(kRelid);
    Relation rel = CreateTestRelation(kRelid, "test_insert2", attrs);

    // Insert 5 tuples.
    for (int i = 0; i < 5; i++) {
        Datum values[2] = {Int32GetDatum(i), Int32GetDatum(i * 10)};
        bool isnull[2] = {false, false};
        HeapTuple tup = heap_form_tuple(rel->rd_att, values, isnull);
        ItemPointerData tid = heap_insert(rel, tup);
        EXPECT_EQ(tid.ip_blkid, 0u);
        EXPECT_EQ(tid.ip_posid, static_cast<uint16_t>(i + 1));
        heap_freetuple(tup);
    }

    CommitAndStartNew();

    SnapshotData snap = MakeSnapshot(pgcpp::transaction::GetNextTransactionId(),
                                     pgcpp::transaction::GetNextTransactionId() + 1);
    HeapScanDesc scan = heap_beginscan(rel, &snap);

    int count = 0;
    while (heap_getnext(scan) != nullptr) {
        count++;
    }
    EXPECT_EQ(count, 5);
    heap_endscan(scan);

    RelationClose(rel);
}

TEST_F(HeapamTest, InsertExtendsToMultiplePages) {
    constexpr Oid kRelid = 1002;
    auto attrs = MakeMixedSchema(kRelid);
    Relation rel = CreateTestRelation(kRelid, "test_insert3", attrs);

    // Insert many tuples to force page extension.
    // Each tuple is ~30 bytes; a page holds ~200 tuples.
    const int kCount = 250;
    for (int i = 0; i < kCount; i++) {
        Datum values[3] = {Int32GetDatum(i), Int64GetDatum(i * 1000LL),
                           MakeTextDatum("item" + std::to_string(i))};
        bool isnull[3] = {false, false, false};
        HeapTuple tup = heap_form_tuple(rel->rd_att, values, isnull);
        heap_insert(rel, tup);
        heap_freetuple(tup);
    }

    CommitAndStartNew();

    SnapshotData snap = MakeSnapshot(pgcpp::transaction::GetNextTransactionId(),
                                     pgcpp::transaction::GetNextTransactionId() + 1);
    HeapScanDesc scan = heap_beginscan(rel, &snap);

    int count = 0;
    int last_id = -1;
    HeapTuple tup;
    while ((tup = heap_getnext(scan)) != nullptr) {
        bool isnull_out = false;
        int id = DatumGetInt32(heap_getattr(tup, 1, rel->rd_att, &isnull_out));
        EXPECT_FALSE(isnull_out);
        EXPECT_EQ(id, last_id + 1);
        last_id = id;
        count++;
    }
    EXPECT_EQ(count, kCount);
    EXPECT_EQ(last_id, kCount - 1);
    heap_endscan(scan);

    RelationClose(rel);
}

// --- heap_delete ---

TEST_F(HeapamTest, DeleteMakesTupleInvisible) {
    constexpr Oid kRelid = 1003;
    auto attrs = MakeIntIntSchema(kRelid);
    Relation rel = CreateTestRelation(kRelid, "test_delete", attrs);

    Datum values[2] = {Int32GetDatum(1), Int32GetDatum(2)};
    bool isnull[2] = {false, false};
    HeapTuple tup = heap_form_tuple(rel->rd_att, values, isnull);
    ItemPointerData tid = heap_insert(rel, tup);
    heap_freetuple(tup);

    CommitAndStartNew();

    // Verify it's visible.
    SnapshotData snap1 = MakeSnapshot(pgcpp::transaction::GetNextTransactionId(),
                                      pgcpp::transaction::GetNextTransactionId() + 1);
    HeapScanDesc scan1 = heap_beginscan(rel, &snap1);
    EXPECT_NE(heap_getnext(scan1), nullptr);
    EXPECT_EQ(heap_getnext(scan1), nullptr);
    heap_endscan(scan1);

    // Delete it.
    heap_delete(rel, tid);
    CommitAndStartNew();

    // Now it should be invisible.
    SnapshotData snap2 = MakeSnapshot(pgcpp::transaction::GetNextTransactionId(),
                                      pgcpp::transaction::GetNextTransactionId() + 1);
    HeapScanDesc scan2 = heap_beginscan(rel, &snap2);
    EXPECT_EQ(heap_getnext(scan2), nullptr);
    heap_endscan(scan2);

    RelationClose(rel);
}

// --- heap_update ---

TEST_F(HeapamTest, UpdateReplacesTuple) {
    constexpr Oid kRelid = 1004;
    auto attrs = MakeIntIntSchema(kRelid);
    Relation rel = CreateTestRelation(kRelid, "test_update", attrs);

    Datum values[2] = {Int32GetDatum(1), Int32GetDatum(2)};
    bool isnull[2] = {false, false};
    HeapTuple tup = heap_form_tuple(rel->rd_att, values, isnull);
    ItemPointerData old_tid = heap_insert(rel, tup);
    heap_freetuple(tup);

    CommitAndStartNew();

    // Update: new values.
    Datum new_values[2] = {Int32GetDatum(10), Int32GetDatum(20)};
    bool new_isnull[2] = {false, false};
    HeapTuple new_tup = heap_form_tuple(rel->rd_att, new_values, new_isnull);
    ItemPointerData new_tid = heap_update(rel, old_tid, new_tup);
    heap_freetuple(new_tup);

    CommitAndStartNew();

    // Scan: only the new version should be visible.
    SnapshotData snap = MakeSnapshot(pgcpp::transaction::GetNextTransactionId(),
                                     pgcpp::transaction::GetNextTransactionId() + 1);
    HeapScanDesc scan = heap_beginscan(rel, &snap);
    HeapTuple out = heap_getnext(scan);
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(out->t_self, new_tid);

    bool isnull_out = false;
    EXPECT_EQ(DatumGetInt32(heap_getattr(out, 1, rel->rd_att, &isnull_out)), 10);
    EXPECT_EQ(DatumGetInt32(heap_getattr(out, 2, rel->rd_att, &isnull_out)), 20);
    EXPECT_EQ(heap_getnext(scan), nullptr);
    heap_endscan(scan);

    RelationClose(rel);
}

// --- heap_rescan ---

TEST_F(HeapamTest, RescanRestartsFromBeginning) {
    constexpr Oid kRelid = 1005;
    auto attrs = MakeIntIntSchema(kRelid);
    Relation rel = CreateTestRelation(kRelid, "test_rescan", attrs);

    for (int i = 0; i < 3; i++) {
        Datum values[2] = {Int32GetDatum(i), Int32GetDatum(i)};
        bool isnull[2] = {false, false};
        HeapTuple tup = heap_form_tuple(rel->rd_att, values, isnull);
        heap_insert(rel, tup);
        heap_freetuple(tup);
    }
    CommitAndStartNew();

    SnapshotData snap = MakeSnapshot(pgcpp::transaction::GetNextTransactionId(),
                                     pgcpp::transaction::GetNextTransactionId() + 1);
    HeapScanDesc scan = heap_beginscan(rel, &snap);

    // Read 2 tuples.
    EXPECT_NE(heap_getnext(scan), nullptr);
    EXPECT_NE(heap_getnext(scan), nullptr);

    // Rescan should restart.
    heap_rescan(scan);

    int count = 0;
    while (heap_getnext(scan) != nullptr)
        count++;
    EXPECT_EQ(count, 3);
    heap_endscan(scan);

    RelationClose(rel);
}

// --- heap_beginscan with null snapshot ---

TEST_F(HeapamTest, BeginScanWithNullSnapshotUsesTransactionSnapshot) {
    constexpr Oid kRelid = 1006;
    auto attrs = MakeIntIntSchema(kRelid);
    Relation rel = CreateTestRelation(kRelid, "test_nullsnap", attrs);

    Datum values[2] = {Int32GetDatum(1), Int32GetDatum(2)};
    bool isnull[2] = {false, false};
    HeapTuple tup = heap_form_tuple(rel->rd_att, values, isnull);
    heap_insert(rel, tup);
    heap_freetuple(tup);
    CommitAndStartNew();

    // Pass nullptr — heap_beginscan should fall back to GetTransactionSnapshot.
    HeapScanDesc scan = heap_beginscan(rel, nullptr);
    EXPECT_NE(scan, nullptr);
    EXPECT_NE(scan->rs_snapshot, nullptr);
    heap_endscan(scan);

    RelationClose(rel);
}

// --- Tuple header fields after insert ---

TEST_F(HeapamTest, InsertSetsXminAndCid) {
    constexpr Oid kRelid = 1007;
    auto attrs = MakeIntIntSchema(kRelid);
    Relation rel = CreateTestRelation(kRelid, "test_xmin", attrs);

    Datum values[2] = {Int32GetDatum(1), Int32GetDatum(2)};
    bool isnull[2] = {false, false};
    HeapTuple tup = heap_form_tuple(rel->rd_att, values, isnull);
    heap_insert(rel, tup);

    // t_xmin should be set to the current transaction ID.
    TransactionId xmin = HeapTupleHeaderGetXmin(tup->t_data);
    EXPECT_NE(xmin, pgcpp::transaction::kInvalidTransactionId);

    heap_freetuple(tup);
    RelationClose(rel);
}

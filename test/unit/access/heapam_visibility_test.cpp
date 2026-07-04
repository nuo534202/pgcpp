// heapam_visibility_test.cpp — Tests for per-page visibility cache invalidation.
//
// Verifies that CommandCounterIncrement (called internally by heap_insert /
// heap_delete) marks all active scans' visibility caches as stale, so that
// subsequent heap_getnext calls rebuild the cache and see prior commands'
// changes. This is the A-4 fix.
//
// The fixture mirrors heapam_test.cpp's HeapamTest (self-contained per project
// convention): error subsystem, memory context, catalog, syscache, transaction
// state, buffer pool, storage dir, relcache.

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "access/heapam.hpp"
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

using pgcpp::access::CreateTupleDesc;
using pgcpp::access::heap_beginscan;
using pgcpp::access::heap_delete;
using pgcpp::access::heap_endscan;
using pgcpp::access::heap_form_tuple;
using pgcpp::access::heap_freetuple;
using pgcpp::access::heap_getnext;
using pgcpp::access::heap_insert;
using pgcpp::access::heap_rescan;
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
using pgcpp::nodes::makePallocNode;
using pgcpp::storage::InitBufferPool;
using pgcpp::storage::SetStorageBaseDir;
using pgcpp::storage::ShutdownBufferPool;
using pgcpp::storage::smgrcloseall;
using pgcpp::transaction::BeginTransactionBlock;
using pgcpp::transaction::CommandCounterIncrement;
using pgcpp::transaction::EndTransactionBlock;
using pgcpp::transaction::HeapTuple;
using pgcpp::transaction::InitializeTransactionSystem;
using pgcpp::transaction::ItemPointerData;
using pgcpp::transaction::kInvalidCommandId;
using pgcpp::transaction::ResetTransactionState;
using pgcpp::types::Datum;
using pgcpp::types::DatumGetInt32;
using pgcpp::types::Int32GetDatum;
using pgcpp::types::kInt4Oid;

namespace {

class HeapamVisibilityTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("heapam_vis_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        syscache_ = new SysCache();
        SetSysCache(syscache_);

        ResetTransactionState();
        InitializeTransactionSystem();
        BeginTransactionBlock();

        test_dir_ = "/tmp/pgcpp_heapam_vis_test_" + std::to_string(getpid());
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

    FormData_pg_class* MakeClassRow(const std::string& name, Oid oid) {
        auto* row = makePallocNode<FormData_pg_class>();
        row->oid = oid;
        row->relname = name;
        row->relfilenode = oid;
        row->relkind = RelKind::kRelation;
        row->relpersistence = RelPersistence::kPermanent;
        return row;
    }

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

    // Insert a tuple with the given int4 values and return its TID.
    ItemPointerData InsertTuple(Relation rel, int32_t a, int32_t b) {
        Datum values[2] = {Int32GetDatum(a), Int32GetDatum(b)};
        bool isnull[2] = {false, false};
        HeapTuple tup = heap_form_tuple(rel->rd_att, values, isnull);
        ItemPointerData tid = heap_insert(rel, tup);
        heap_freetuple(tup);
        return tid;
    }

    // Count visible tuples in a fresh scan (uses the transaction snapshot).
    int CountVisibleTuples(Relation rel) {
        HeapScanDesc scan = heap_beginscan(rel, nullptr);
        int count = 0;
        while (heap_getnext(scan) != nullptr) {
            count++;
        }
        heap_endscan(scan);
        return count;
    }

    // Collect the first column (int4) of all visible tuples in a fresh scan.
    std::vector<int32_t> CollectFirstColumn(Relation rel, TupleDesc desc) {
        HeapScanDesc scan = heap_beginscan(rel, nullptr);
        std::vector<int32_t> result;
        HeapTuple tup;
        while ((tup = heap_getnext(scan)) != nullptr) {
            bool isnull = false;
            Datum d = pgcpp::access::heap_getattr(tup, 1, desc, &isnull);
            if (!isnull) {
                result.push_back(DatumGetInt32(d));
            }
        }
        heap_endscan(scan);
        return result;
    }

    void CommitAndStartNew() {
        EndTransactionBlock();
        BeginTransactionBlock();
    }

    static void RunShell(const std::string& cmd) { std::system(cmd.c_str()); }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
    SysCache* syscache_ = nullptr;
    std::string test_dir_;
};

// --- Cache invalidation tests ---

// CCI (called internally by heap_insert) marks active scans' caches as stale.
// After inserting T1 and starting a scan (which builds the cache), inserting
// T2 triggers CCI which must set rs_cached_cnum back to kInvalidCommandId.
TEST_F(HeapamVisibilityTest, CCIInvalidatesVisibilityCache) {
    constexpr Oid kRelid = 2001;
    auto attrs = MakeIntIntSchema(kRelid);
    Relation rel = CreateTestRelation(kRelid, "vis_test1", attrs);

    // Insert T1 (heap_insert calls CCI → command ID advances to 1).
    InsertTuple(rel, 1, 10);

    // Start a scan and fetch T1 (builds the visibility cache).
    HeapScanDesc scan = heap_beginscan(rel, nullptr);
    ASSERT_NE(heap_getnext(scan), nullptr);
    // Cache should now be marked valid (rs_cached_cnum == current command ID).
    EXPECT_NE(scan->rs_cached_cnum, kInvalidCommandId);

    // Insert T2 (heap_insert calls CCI → InvalidateAllVisibilityCaches).
    InsertTuple(rel, 2, 20);

    // The scan's cache must have been invalidated.
    EXPECT_EQ(scan->rs_cached_cnum, kInvalidCommandId);

    heap_endscan(scan);
    RelationClose(rel);
}

// Without any DML/CCI, the cache stays valid across multiple heap_getnext calls.
TEST_F(HeapamVisibilityTest, CacheStaysValidWithoutCCI) {
    constexpr Oid kRelid = 2002;
    auto attrs = MakeIntIntSchema(kRelid);
    Relation rel = CreateTestRelation(kRelid, "vis_test2", attrs);

    InsertTuple(rel, 1, 10);
    InsertTuple(rel, 2, 20);

    HeapScanDesc scan = heap_beginscan(rel, nullptr);
    ASSERT_NE(heap_getnext(scan), nullptr);

    // Record the cached command ID.
    auto cached = scan->rs_cached_cnum;
    EXPECT_NE(cached, kInvalidCommandId);

    // Call heap_getnext again without any CCI in between.
    heap_getnext(scan);

    // Cache should still be valid (same command ID).
    EXPECT_EQ(scan->rs_cached_cnum, cached);

    heap_endscan(scan);
    RelationClose(rel);
}

// After inserting tuples + CCI, a fresh scan sees all inserted tuples.
// (The scan uses the transaction snapshot, which has curcid >= the tuples'
// t_cmin because GetTransactionSnapshot is called after the inserts' CCIs.)
TEST_F(HeapamVisibilityTest, FreshScanSeesAllInsertedTuples) {
    constexpr Oid kRelid = 2003;
    auto attrs = MakeIntIntSchema(kRelid);
    Relation rel = CreateTestRelation(kRelid, "vis_test3", attrs);

    InsertTuple(rel, 1, 10);
    InsertTuple(rel, 2, 20);
    InsertTuple(rel, 3, 30);

    // A fresh scan should see all 3 tuples.
    EXPECT_EQ(CountVisibleTuples(rel), 3);

    RelationClose(rel);
}

// After deleting a tuple + CCI + commit, a fresh scan no longer sees it.
TEST_F(HeapamVisibilityTest, DeletedTupleInvisibleAfterCommit) {
    constexpr Oid kRelid = 2004;
    auto attrs = MakeIntIntSchema(kRelid);
    Relation rel = CreateTestRelation(kRelid, "vis_test4", attrs);

    ItemPointerData tid1 = InsertTuple(rel, 1, 10);
    InsertTuple(rel, 2, 20);

    // Commit so the inserting XID is committed, then start a new txn.
    CommitAndStartNew();

    // Both tuples visible.
    EXPECT_EQ(CountVisibleTuples(rel), 2);

    // Delete T1 (heap_delete calls CCI).
    heap_delete(rel, tid1);

    // Commit so the deleting XID is committed, then start a new txn.
    CommitAndStartNew();

    // Only T2 should be visible.
    EXPECT_EQ(CountVisibleTuples(rel), 1);

    // Verify the surviving tuple has value 2.
    auto values = CollectFirstColumn(rel, rel->rd_att);
    ASSERT_EQ(values.size(), 1u);
    EXPECT_EQ(values[0], 2);

    RelationClose(rel);
}

// heap_rescan invalidates the cache so the scan sees the latest state.
TEST_F(HeapamVisibilityTest, RescanSeesNewTuplesAfterInsert) {
    constexpr Oid kRelid = 2005;
    auto attrs = MakeIntIntSchema(kRelid);
    Relation rel = CreateTestRelation(kRelid, "vis_test5", attrs);

    InsertTuple(rel, 1, 10);
    InsertTuple(rel, 2, 20);

    // Start scan, count 2 tuples.
    HeapScanDesc scan = heap_beginscan(rel, nullptr);
    int count = 0;
    while (heap_getnext(scan) != nullptr) {
        count++;
    }
    EXPECT_EQ(count, 2);

    // Insert T3 (heap_insert calls CCI → cache invalidated).
    InsertTuple(rel, 3, 30);

    // Rescan should see all 3 tuples.
    heap_rescan(scan);
    count = 0;
    while (heap_getnext(scan) != nullptr) {
        count++;
    }
    EXPECT_EQ(count, 3);

    heap_endscan(scan);
    RelationClose(rel);
}

}  // namespace

// vacuum_test.cpp — Tests for VACUUM space reclamation.
//
// Verifies that ExecVacuum reclaims dead tuples (marking them LP_DEAD and
// compacting pages via PageRepairFragmentation) and preserves live tuples.
// This is the A-4 fix: DML marks tuples dead, VACUUM reclaims space.
//
// The fixture extends HeapamTest with ProcArray management: GetOldestXmin()
// must return a value > the deleting XID for HeapTupleIsSurelyDead to return
// true. This requires the current transaction to be registered in ProcArray.

#include "commands/vacuum.hpp"

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
#include "parser/parsenodes.hpp"
#include "storage/bufmgr.hpp"
#include "storage/bufpage.hpp"
#include "storage/smgr.hpp"
#include "transaction/heap_tuple.hpp"
#include "transaction/procarray.hpp"
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
using pgcpp::access::heap_getattr;
using pgcpp::access::heap_getnext;
using pgcpp::access::heap_insert;
using pgcpp::access::HeapScanDesc;
using pgcpp::access::InitializeRelcache;
using pgcpp::access::Relation;
using pgcpp::access::RelationClose;
using pgcpp::access::RelationCreateStorage;
using pgcpp::access::RelationGetNumberOfBlocks;
using pgcpp::access::RelationGetSmgr;
using pgcpp::access::RelationOpen;
using pgcpp::access::ResetRelcache;
using pgcpp::access::TupleDesc;
using pgcpp::catalog::AttAlign;
using pgcpp::catalog::AttStorage;
using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_attribute;
using pgcpp::catalog::FormData_pg_class;
using pgcpp::catalog::kInvalidOid;
using pgcpp::catalog::Oid;
using pgcpp::catalog::RelKind;
using pgcpp::catalog::RelPersistence;
using pgcpp::catalog::SetCatalog;
using pgcpp::catalog::SetSysCache;
using pgcpp::catalog::SysCache;
using pgcpp::commands::ExecVacuum;
using pgcpp::memory::AllocSetContext;
using pgcpp::nodes::makePallocNode;
using pgcpp::parser::RangeVar;
using pgcpp::parser::VacuumStmt;
using pgcpp::storage::Buffer;
using pgcpp::storage::BufferGetPage;
using pgcpp::storage::ForkNumber;
using pgcpp::storage::InitBufferPool;
using pgcpp::storage::PageGetHeapFreeSpace;
using pgcpp::storage::ReadBuffer;
using pgcpp::storage::ReadBufferMode;
using pgcpp::storage::ReleaseBuffer;
using pgcpp::storage::SetStorageBaseDir;
using pgcpp::storage::ShutdownBufferPool;
using pgcpp::storage::smgrcloseall;
using pgcpp::transaction::BeginTransactionBlock;
using pgcpp::transaction::EndTransactionBlock;
using pgcpp::transaction::GetCurrentTransactionId;
using pgcpp::transaction::GetCurrentTransactionIdIfAny;
using pgcpp::transaction::GetOldestXmin;
using pgcpp::transaction::HeapTuple;
using pgcpp::transaction::InitializeProcArray;
using pgcpp::transaction::InitializeTransactionSystem;
using pgcpp::transaction::ItemPointerData;
using pgcpp::transaction::kInvalidTransactionId;
using pgcpp::transaction::ProcArrayAdd;
using pgcpp::transaction::ProcArrayRemove;
using pgcpp::transaction::ResetProcArray;
using pgcpp::transaction::ResetTransactionState;
using pgcpp::transaction::TransactionId;
using pgcpp::transaction::TransactionIdIsValid;
using pgcpp::types::Datum;
using pgcpp::types::DatumGetInt32;
using pgcpp::types::Int32GetDatum;
using pgcpp::types::kInt4Oid;

namespace {

class VacuumTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("vacuum_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        syscache_ = new SysCache();
        SetSysCache(syscache_);

        ResetTransactionState();
        InitializeTransactionSystem();
        InitializeProcArray();
        BeginTransactionBlock();

        // Force XID assignment and register in ProcArray so GetOldestXmin()
        // returns a meaningful value (needed by HeapTupleIsSurelyDead).
        current_xid_ = GetCurrentTransactionId();
        ProcArrayAdd(current_xid_);

        test_dir_ = "/tmp/pgcpp_vacuum_test_" + std::to_string(getpid());
        SetStorageBaseDir(test_dir_);
        RunShell("rm -rf " + test_dir_);

        InitBufferPool(64);
        InitializeRelcache();
    }

    void TearDown() override {
        // Remove the current XID from ProcArray before ending the transaction.
        if (TransactionIdIsValid(current_xid_)) {
            ProcArrayRemove(current_xid_);
        }

        EndTransactionBlock();
        ResetRelcache();
        ShutdownBufferPool();
        smgrcloseall();
        RunShell("rm -rf " + test_dir_);

        SetSysCache(nullptr);
        SetCatalog(nullptr);
        delete syscache_;
        delete catalog_;

        ResetProcArray();
        ResetTransactionState();
        InitializeTransactionSystem();

        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    // Commit the current transaction and start a new one, updating ProcArray.
    // The old XID is committed (in CLOG via EndTransactionBlock) and removed
    // from ProcArray; the new XID is allocated and added to ProcArray.
    void CommitAndStartNew() {
        TransactionId old_xid = GetCurrentTransactionIdIfAny();
        EndTransactionBlock();
        if (TransactionIdIsValid(old_xid)) {
            ProcArrayRemove(old_xid);
        }
        BeginTransactionBlock();
        current_xid_ = GetCurrentTransactionId();
        ProcArrayAdd(current_xid_);
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

    ItemPointerData InsertTuple(Relation rel, int32_t a, int32_t b) {
        Datum values[2] = {Int32GetDatum(a), Int32GetDatum(b)};
        bool isnull[2] = {false, false};
        HeapTuple tup = heap_form_tuple(rel->rd_att, values, isnull);
        ItemPointerData tid = heap_insert(rel, tup);
        heap_freetuple(tup);
        return tid;
    }

    // Build a VacuumStmt targeting the given relation name.
    VacuumStmt* MakeVacuumStmt(const std::string& relname) {
        auto* stmt = makePallocNode<VacuumStmt>();
        auto* rv = makePallocNode<RangeVar>();
        rv->relname = relname;
        stmt->rels.push_back(rv);
        stmt->is_vacuumcmd = true;
        return stmt;
    }

    // Get free space on the first page of a relation.
    int GetPageFreeSpace(Relation rel) {
        rel->rd_smgr = RelationGetSmgr(rel);
        Buffer buf = ReadBuffer(rel->rd_smgr, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
        int space = PageGetHeapFreeSpace(BufferGetPage(buf));
        ReleaseBuffer(buf);
        return space;
    }

    // Count visible tuples in a fresh scan.
    int CountVisibleTuples(Relation rel) {
        HeapScanDesc scan = heap_beginscan(rel, nullptr);
        int count = 0;
        while (heap_getnext(scan) != nullptr) {
            count++;
        }
        heap_endscan(scan);
        return count;
    }

    // Collect the first column (int4) of all visible tuples.
    std::vector<int32_t> CollectFirstColumn(Relation rel, TupleDesc desc) {
        HeapScanDesc scan = heap_beginscan(rel, nullptr);
        std::vector<int32_t> result;
        HeapTuple tup;
        while ((tup = heap_getnext(scan)) != nullptr) {
            bool isnull = false;
            Datum d = heap_getattr(tup, 1, desc, &isnull);
            if (!isnull) {
                result.push_back(DatumGetInt32(d));
            }
        }
        heap_endscan(scan);
        return result;
    }

    static void RunShell(const std::string& cmd) { std::system(cmd.c_str()); }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
    SysCache* syscache_ = nullptr;
    std::string test_dir_;
    TransactionId current_xid_ = kInvalidTransactionId;
};

// --- VACUUM tests ---

// VACUUM reclaims dead tuples: after deleting some tuples and committing,
// VACUUM compacts the page, increasing free space. Live tuples are preserved.
TEST_F(VacuumTest, VacuumReclaimsDeadTuples) {
    constexpr Oid kRelid = 3001;
    auto attrs = MakeIntIntSchema(kRelid);
    Relation rel = CreateTestRelation(kRelid, "vac_test1", attrs);

    // Insert 5 tuples.
    std::vector<ItemPointerData> tids;
    for (int i = 1; i <= 5; i++) {
        tids.push_back(InsertTuple(rel, i, i * 10));
    }

    // Commit so the inserting XID is committed.
    CommitAndStartNew();

    // Delete tuples 2 and 4.
    heap_delete(rel, tids[1]);  // tuple with a=2
    heap_delete(rel, tids[3]);  // tuple with a=4

    // Commit so the deleting XID is committed (HeapTupleIsSurelyDead can see it).
    CommitAndStartNew();

    // Record free space before VACUUM.
    int space_before = GetPageFreeSpace(rel);

    // Run VACUUM.
    std::string result = ExecVacuum(MakeVacuumStmt("vac_test1"));
    EXPECT_EQ(result, "VACUUM");

    // Record free space after VACUUM — should increase (dead tuples reclaimed).
    int space_after = GetPageFreeSpace(rel);
    EXPECT_GT(space_after, space_before);

    // Only tuples 1, 3, 5 should remain.
    EXPECT_EQ(CountVisibleTuples(rel), 3);
    auto values = CollectFirstColumn(rel, rel->rd_att);
    ASSERT_EQ(values.size(), 3u);
    EXPECT_EQ(values[0], 1);
    EXPECT_EQ(values[1], 3);
    EXPECT_EQ(values[2], 5);

    RelationClose(rel);
}

// VACUUM on a clean table (no dead tuples) is a no-op: free space unchanged.
TEST_F(VacuumTest, VacuumNoOpOnCleanTable) {
    constexpr Oid kRelid = 3002;
    auto attrs = MakeIntIntSchema(kRelid);
    Relation rel = CreateTestRelation(kRelid, "vac_test2", attrs);

    InsertTuple(rel, 1, 10);
    InsertTuple(rel, 2, 20);
    InsertTuple(rel, 3, 30);

    CommitAndStartNew();

    int space_before = GetPageFreeSpace(rel);

    std::string result = ExecVacuum(MakeVacuumStmt("vac_test2"));
    EXPECT_EQ(result, "VACUUM");

    int space_after = GetPageFreeSpace(rel);
    EXPECT_EQ(space_after, space_before);

    // All 3 tuples still visible.
    EXPECT_EQ(CountVisibleTuples(rel), 3);

    RelationClose(rel);
}

// VACUUM preserves live tuples when some are deleted.
TEST_F(VacuumTest, VacuumPreservesLiveTuples) {
    constexpr Oid kRelid = 3003;
    auto attrs = MakeIntIntSchema(kRelid);
    Relation rel = CreateTestRelation(kRelid, "vac_test3", attrs);

    // Insert 10 tuples (a = 1..10).
    std::vector<ItemPointerData> tids;
    for (int i = 1; i <= 10; i++) {
        tids.push_back(InsertTuple(rel, i, i * 10));
    }

    CommitAndStartNew();

    // Delete the even-numbered tuples (a = 2, 4, 6, 8, 10).
    for (int i = 2; i <= 10; i += 2) {
        heap_delete(rel, tids[i - 1]);
    }

    CommitAndStartNew();

    // VACUUM should reclaim the 5 dead tuples.
    std::string result = ExecVacuum(MakeVacuumStmt("vac_test3"));
    EXPECT_EQ(result, "VACUUM");

    // 5 live tuples should remain (odd a values).
    EXPECT_EQ(CountVisibleTuples(rel), 5);
    auto values = CollectFirstColumn(rel, rel->rd_att);
    ASSERT_EQ(values.size(), 5u);
    for (int v : values) {
        EXPECT_EQ(v % 2, 1);  // all odd
    }

    RelationClose(rel);
}

}  // namespace

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
#include "server/autovacuum.hpp"
#include "storage/bufmgr.hpp"
#include "storage/bufpage.hpp"
#include "storage/ipc/proc.hpp"
#include "storage/smgr.hpp"
#include "transaction/heap_tuple.hpp"
#include "transaction/procarray.hpp"
#include "transaction/snapshot.hpp"
#include "transaction/transam.hpp"
#include "transaction/visibility.hpp"
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
using pgcpp::commands::RelationNeedsVacuumForWraparound;
using pgcpp::commands::VacuumGetFreezeLimit;
using pgcpp::commands::VacuumStats;
using pgcpp::memory::AllocSetContext;
using pgcpp::nodes::makePallocNode;
using pgcpp::parser::RangeVar;
using pgcpp::parser::VacuumStmt;
using pgcpp::server::AutoVacuumLauncherMain;
using pgcpp::server::AutoVacuumStats;
using pgcpp::server::AutoVacuumWorkItem;
using pgcpp::server::GetAutoVacuumStats;
using pgcpp::server::InitializeAutoVacuum;
using pgcpp::server::RegisterAutoVacuumWorkItem;
using pgcpp::storage::Buffer;
using pgcpp::storage::BufferGetPage;
using pgcpp::storage::ForkNumber;
using pgcpp::storage::InitBufferPool;
using pgcpp::storage::InitProcess;
using pgcpp::storage::OffsetNumber;
using pgcpp::storage::Page;
using pgcpp::storage::PageGetHeapFreeSpace;
using pgcpp::storage::ProcKill;
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
using pgcpp::transaction::HeapTupleHeaderData;
using pgcpp::transaction::HeapTupleHeaderGetXmin;
using pgcpp::transaction::HeapTupleHeaderGetXminStatus;
using pgcpp::transaction::InitializeProcArray;
using pgcpp::transaction::InitializeTransactionSystem;
using pgcpp::transaction::ItemPointerData;
using pgcpp::transaction::kFrozenTransactionId;
using pgcpp::transaction::kInvalidTransactionId;
using pgcpp::transaction::ResetProcArray;
using pgcpp::transaction::ResetTransactionState;
using pgcpp::transaction::TransactionId;
using pgcpp::transaction::TransactionIdIsValid;
using pgcpp::transaction::XactStatus;
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
        // P0-3: claim a PGPROC slot (registers in ProcArray) so the backend
        // is visible to GetOldestXmin / snapshot scans.
        InitProcess();
        BeginTransactionBlock();

        // Force XID assignment — GetCurrentTransactionId publishes the XID
        // in PGXACT so GetOldestXmin() returns a meaningful value (needed
        // by HeapTupleIsSurelyDead).
        current_xid_ = GetCurrentTransactionId();

        test_dir_ = "/tmp/pgcpp_vacuum_test_" + std::to_string(getpid());
        SetStorageBaseDir(test_dir_);
        RunShell("rm -rf " + test_dir_);

        InitBufferPool(64);
        InitializeRelcache();
        InitializeAutoVacuum();
    }

    void TearDown() override {
        EndTransactionBlock();
        // P0-3: release the PGPROC slot (deregisters from ProcArray) BEFORE
        // ShutdownBufferPool(), which calls ResetShmem() and frees the
        // ProcArray/PGPROC backing memory.
        ProcKill();
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

    // Commit the current transaction and start a new one. CommitTransaction
    // clears the old XID from PGXACT; the new GetCurrentTransactionId call
    // publishes the new XID. No manual ProcArray manipulation needed.
    void CommitAndStartNew() {
        EndTransactionBlock();
        BeginTransactionBlock();
        current_xid_ = GetCurrentTransactionId();
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

    // Build a VacuumStmt with FREEZE option targeting the given relation.
    VacuumStmt* MakeVacuumFreezeStmt(const std::string& relname) {
        auto* stmt = MakeVacuumStmt(relname);
        stmt->freeze = true;
        return stmt;
    }

    // Read the t_xmin of the first live tuple on page 0 of the relation.
    // Returns kInvalidTransactionId if no live tuple is found.
    TransactionId GetFirstTupleXmin(Relation rel) {
        rel->rd_smgr = RelationGetSmgr(rel);
        Buffer buf = ReadBuffer(rel->rd_smgr, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
        Page page = BufferGetPage(buf);
        OffsetNumber max_off = pgcpp::storage::PageGetMaxOffsetNumber(page);
        TransactionId xmin = kInvalidTransactionId;
        for (OffsetNumber off = 1; off <= max_off; ++off) {
            auto* item_id = pgcpp::storage::PageGetItemId(page, off);
            if (!pgcpp::storage::ItemIdIsNormal(item_id)) {
                continue;
            }
            auto* header =
                reinterpret_cast<HeapTupleHeaderData*>(pgcpp::storage::PageGetItem(page, item_id));
            xmin = HeapTupleHeaderGetXmin(header);
            break;
        }
        ReleaseBuffer(buf);
        return xmin;
    }

    // Read the relfrozenxid of a relation from pg_class via the catalog.
    TransactionId GetRelfrozenxid(Oid relid) {
        const pgcpp::catalog::FormData_pg_class* row = catalog_->GetClassByOid(relid);
        return (row != nullptr) ? row->relfrozenxid : kInvalidTransactionId;
    }

    // Set the relfrozenxid of a relation (for testing wraparound detection).
    void SetRelfrozenxid(Oid relid, TransactionId frozenxid) {
        const pgcpp::catalog::FormData_pg_class* row = catalog_->GetClassByOid(relid);
        ASSERT_NE(row, nullptr);
        auto* updated = makePallocNode<pgcpp::catalog::FormData_pg_class>(*row);
        updated->relfrozenxid = frozenxid;
        catalog_->UpdateClass(relid, updated);
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

    static void RunShell(const std::string& cmd) {
        int rc = std::system(cmd.c_str());
        (void)rc;
    }

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

// --- VACUUM FREEZE tests ---

// VACUUM FREEZE replaces t_xmin of committed tuples with FrozenTransactionId.
TEST_F(VacuumTest, VacuumFreezeReplacesXmin) {
    constexpr Oid kRelid = 3004;
    auto attrs = MakeIntIntSchema(kRelid);
    Relation rel = CreateTestRelation(kRelid, "vac_freeze1", attrs);

    InsertTuple(rel, 1, 10);
    InsertTuple(rel, 2, 20);

    // Commit so the inserting XID is committed.
    CommitAndStartNew();

    // Record the original xmin (should be a normal XID, not Frozen).
    TransactionId xmin_before = GetFirstTupleXmin(rel);
    ASSERT_TRUE(TransactionIdIsValid(xmin_before));
    EXPECT_NE(xmin_before, kFrozenTransactionId);

    // Run VACUUM FREEZE.
    VacuumStats stats;
    std::string result = ExecVacuum(MakeVacuumFreezeStmt("vac_freeze1"), &stats);
    EXPECT_EQ(result, "VACUUM");
    EXPECT_GT(stats.tuples_frozen, 0);

    // After FREEZE, the tuple's xmin should be FrozenTransactionId.
    TransactionId xmin_after = GetFirstTupleXmin(rel);
    EXPECT_EQ(xmin_after, kFrozenTransactionId);

    // Tuples are still visible.
    EXPECT_EQ(CountVisibleTuples(rel), 2);

    RelationClose(rel);
}

// VACUUM (without FREEZE) at low XIDs still freezes conservatively: when
// oldest_xmin is too small to subtract vacuum_freeze_min_age (underflow
// guard), the freeze limit defaults to oldest_xmin, so tuples committed by
// previous transactions (xmin < oldest_xmin) get frozen. This matches
// PostgreSQL's conservative behavior at low XID values.
TEST_F(VacuumTest, VacuumWithoutFreezeAtLowXidsFreezesConservatively) {
    constexpr Oid kRelid = 3005;
    auto attrs = MakeIntIntSchema(kRelid);
    Relation rel = CreateTestRelation(kRelid, "vac_nofreeze1", attrs);

    InsertTuple(rel, 1, 10);

    CommitAndStartNew();

    TransactionId xmin_before = GetFirstTupleXmin(rel);
    ASSERT_TRUE(TransactionIdIsValid(xmin_before));
    EXPECT_NE(xmin_before, kFrozenTransactionId);

    // Normal VACUUM (no FREEZE): at low XIDs the underflow guard sets
    // freeze_limit = oldest_xmin. The tuple's xmin (from the previous,
    // committed transaction) precedes oldest_xmin (current transaction),
    // so it gets frozen conservatively.
    VacuumStats stats;
    std::string result = ExecVacuum(MakeVacuumStmt("vac_nofreeze1"), &stats);
    EXPECT_EQ(result, "VACUUM");
    EXPECT_GT(stats.tuples_frozen, 0);

    // xmin replaced with FrozenTransactionId.
    TransactionId xmin_after = GetFirstTupleXmin(rel);
    EXPECT_EQ(xmin_after, kFrozenTransactionId);

    RelationClose(rel);
}

// VACUUM FREEZE advances pg_class.relfrozenxid to the freeze limit.
TEST_F(VacuumTest, VacuumFreezeAdvancesRelfrozenxid) {
    constexpr Oid kRelid = 3006;
    auto attrs = MakeIntIntSchema(kRelid);
    Relation rel = CreateTestRelation(kRelid, "vac_freeze2", attrs);

    InsertTuple(rel, 1, 10);
    InsertTuple(rel, 2, 20);

    CommitAndStartNew();

    // Before VACUUM, relfrozenxid is 0 (not set by CreateTestRelation).
    TransactionId frozen_before = GetRelfrozenxid(kRelid);

    VacuumStats stats;
    ExecVacuum(MakeVacuumFreezeStmt("vac_freeze2"), &stats);
    EXPECT_TRUE(stats.relfrozenxid_advanced);

    // After VACUUM FREEZE, relfrozenxid should be advanced to the freeze
    // limit (== OldestXmin for aggressive freeze).
    TransactionId frozen_after = GetRelfrozenxid(kRelid);
    EXPECT_NE(frozen_after, frozen_before);
    EXPECT_TRUE(TransactionIdIsValid(frozen_after));

    RelationClose(rel);
}

// VACUUM FREEZE on a frozen tuple is idempotent (no re-freeze needed).
TEST_F(VacuumTest, VacuumFreezeIsIdempotent) {
    constexpr Oid kRelid = 3007;
    auto attrs = MakeIntIntSchema(kRelid);
    Relation rel = CreateTestRelation(kRelid, "vac_freeze3", attrs);

    InsertTuple(rel, 1, 10);
    CommitAndStartNew();

    // First FREEZE.
    VacuumStats stats1;
    ExecVacuum(MakeVacuumFreezeStmt("vac_freeze3"), &stats1);
    EXPECT_GT(stats1.tuples_frozen, 0);

    // Second FREEZE — tuple is already frozen, so tuples_frozen == 0.
    VacuumStats stats2;
    ExecVacuum(MakeVacuumFreezeStmt("vac_freeze3"), &stats2);
    EXPECT_EQ(stats2.tuples_frozen, 0);

    // Tuple is still visible.
    EXPECT_EQ(CountVisibleTuples(rel), 1);

    RelationClose(rel);
}

// --- Freeze-limit computation tests (pure function) ---

TEST_F(VacuumTest, FreezeLimitAggressiveReturnsOldestXmin) {
    TransactionId oldest = 1000;
    EXPECT_EQ(VacuumGetFreezeLimit(oldest, /*aggressive=*/true), oldest);
}

TEST_F(VacuumTest, FreezeLimitNormalAppliesMinAge) {
    // With oldest_xmin large enough, freeze_limit = oldest_xmin - min_age.
    TransactionId oldest = static_cast<TransactionId>(60000000);
    TransactionId expected = oldest - static_cast<TransactionId>(50000000);
    EXPECT_EQ(VacuumGetFreezeLimit(oldest, /*aggressive=*/false), expected);
}

TEST_F(VacuumTest, FreezeLimitNormalNoUnderflowAtLowXids) {
    // At low XIDs (near FirstNormal), the min_age subtraction would underflow;
    // the function returns oldest_xmin to avoid freezing (no tuples qualify).
    TransactionId oldest = 10;
    EXPECT_EQ(VacuumGetFreezeLimit(oldest, /*aggressive=*/false), oldest);
}

// --- Wraparound detection tests ---

// A fresh relation with relfrozenxid=0 is not at wraparound risk.
TEST_F(VacuumTest, WraparoundNotAtRiskForFreshRelation) {
    constexpr Oid kRelid = 3008;
    auto attrs = MakeIntIntSchema(kRelid);
    CreateTestRelation(kRelid, "vac_wrap1", attrs);

    // relfrozenxid defaults to 0 (invalid) — not at risk.
    EXPECT_FALSE(RelationNeedsVacuumForWraparound(kRelid));
}

// A relation whose relfrozenxid is very old (modularly) is at wraparound risk.
TEST_F(VacuumTest, WraparoundAtRiskForOldRelfrozenxid) {
    constexpr Oid kRelid = 3009;
    auto attrs = MakeIntIntSchema(kRelid);
    CreateTestRelation(kRelid, "vac_wrap2", attrs);

    // Simulate wraparound proximity: set relfrozenxid to a value whose
    // modular age (nextXid - relfrozenxid) exceeds kVacuumFreezeMaxAge.
    // With a small nextXid, setting relfrozenxid ahead of nextXid makes
    // the modular difference huge (close to 2^32).
    TransactionId next_xid = pgcpp::transaction::GetNextTransactionId() + 1;
    // Place relfrozenxid just ahead of nextXid so modular age ≈ 2^32 - 1.
    SetRelfrozenxid(kRelid, next_xid + 1);
    EXPECT_TRUE(RelationNeedsVacuumForWraparound(kRelid));
}

// Wraparound detection returns false for a recently-vacuumed relation.
TEST_F(VacuumTest, WraparoundNotAtRiskAfterVacuumFreeze) {
    constexpr Oid kRelid = 3010;
    auto attrs = MakeIntIntSchema(kRelid);
    Relation rel = CreateTestRelation(kRelid, "vac_wrap3", attrs);

    InsertTuple(rel, 1, 10);
    CommitAndStartNew();

    // VACUUM FREEZE advances relfrozenxid to ~oldest_xmin (small value),
    // so the age is small — not at risk.
    ExecVacuum(MakeVacuumFreezeStmt("vac_wrap3"));
    EXPECT_FALSE(RelationNeedsVacuumForWraparound(kRelid));

    RelationClose(rel);
}

// --- Autovacuum integration tests ---

// Autovacuum worker reclaims dead tuples when given a work item targeting
// a real table. Verifies the end-to-end path: RegisterAutoVacuumWorkItem →
// AutoVacuumLauncherMain → AutoVacuumWorkerMain → ExecVacuum.
TEST_F(VacuumTest, AutoVacuumReclaimsDeadTuples) {
    constexpr Oid kRelid = 3011;
    auto attrs = MakeIntIntSchema(kRelid);
    Relation rel = CreateTestRelation(kRelid, "av_test1", attrs);

    // Insert 5 tuples, delete 2, commit.
    std::vector<ItemPointerData> tids;
    for (int i = 1; i <= 5; i++) {
        tids.push_back(InsertTuple(rel, i, i * 10));
    }
    CommitAndStartNew();

    heap_delete(rel, tids[1]);  // a=2
    heap_delete(rel, tids[3]);  // a=4
    CommitAndStartNew();

    int space_before = GetPageFreeSpace(rel);

    // Queue an autovacuum work item for this table and run the launcher.
    AutoVacuumWorkItem item;
    item.database = "testdb";
    item.table = "av_test1";
    item.is_vacuum = true;
    EXPECT_TRUE(RegisterAutoVacuumWorkItem(item));

    int launched = AutoVacuumLauncherMain(/*max_workers=*/1);
    EXPECT_EQ(launched, 1);

    // Stats should reflect one successful VACUUM.
    AutoVacuumStats stats = GetAutoVacuumStats();
    EXPECT_EQ(stats.workers_launched, 1u);
    EXPECT_EQ(stats.vacuums_run, 1u);

    // Dead tuples reclaimed: free space increased, 3 live tuples remain.
    int space_after = GetPageFreeSpace(rel);
    EXPECT_GT(space_after, space_before);
    EXPECT_EQ(CountVisibleTuples(rel), 3);

    RelationClose(rel);
}

// Autovacuum with freeze=true passes the FREEZE flag through to ExecVacuum,
// causing tuples to be frozen.
TEST_F(VacuumTest, AutoVacuumFreezePassesFreezeFlag) {
    constexpr Oid kRelid = 3012;
    auto attrs = MakeIntIntSchema(kRelid);
    Relation rel = CreateTestRelation(kRelid, "av_freeze1", attrs);

    InsertTuple(rel, 1, 10);
    CommitAndStartNew();

    // Before autovacuum, xmin is a normal XID.
    TransactionId xmin_before = GetFirstTupleXmin(rel);
    ASSERT_NE(xmin_before, kFrozenTransactionId);

    // Queue an autovacuum FREEZE work item.
    AutoVacuumWorkItem item;
    item.database = "testdb";
    item.table = "av_freeze1";
    item.is_vacuum = true;
    item.freeze = true;
    RegisterAutoVacuumWorkItem(item);

    AutoVacuumLauncherMain(/*max_workers=*/1);

    // After autovacuum FREEZE, xmin should be FrozenTransactionId.
    TransactionId xmin_after = GetFirstTupleXmin(rel);
    EXPECT_EQ(xmin_after, kFrozenTransactionId);

    RelationClose(rel);
}

}  // namespace

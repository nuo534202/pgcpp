// update_visibility_test.cpp — P0 regression tests for UPDATE through the
// planner.
//
// Verifies the three-part fix for "UPDATE not working" (returned UPDATE 0
// and destroyed data to all NULL):
//   - src/parser/parse_target.cpp (expandUpdateTargetList) — expands the
//     SET target list to include ALL table columns so non-SET columns are
//     preserved.
//   - src/optimizer/plan/planner.cpp — populates ModifyTable's targetlist
//     with Var nodes referencing the child plan's output for UPDATE.
//   - src/executor/node_modify_table.cpp — UPDATE branch returns
//     ps_ResultTupleSlot per affected row so the protocol layer can count
//     them for the "UPDATE N" command tag.
//
// Like insert_select_visibility_test.cpp, these tests build a Query tree
// and call pgcpp::optimizer::planner() so the planner's UPDATE path is
// exercised end-to-end. The target list is built with ALL columns (SET
// column carries the SET expression, non-SET column carries a Var), which
// mirrors what expandUpdateTargetList produces after the parser fix.
//
// Coverage:
//   1. INSERT single row → UPDATE → SELECT sees updated value
//   2. INSERT multiple rows → UPDATE WHERE matches partial rows →
//      correct affected count and only matched rows changed
//   3. UPDATE all rows (no WHERE) → all rows updated, count = N
//   4. UPDATE WHERE not matching → returns 0, data unchanged

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "access/heapam.hpp"
#include "access/rel.hpp"
#include "catalog/bootstrap_catalog.hpp"
#include "catalog/catalog.hpp"
#include "catalog/pg_attribute.hpp"
#include "catalog/pg_class.hpp"
#include "catalog/syscache.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "executor/exec_main.hpp"
#include "executor/node_exec.hpp"
#include "executor/plannodes.hpp"
#include "executor/tupletable.hpp"
#include "optimizer/planner.hpp"
#include "parser/parsenodes.hpp"
#include "parser/primnodes.hpp"
#include "storage/bufmgr.hpp"
#include "storage/smgr.hpp"
#include "transaction/heap_tuple.hpp"
#include "transaction/snapshot.hpp"
#include "transaction/transam.hpp"
#include "transaction/xact.hpp"
#include "types/datum.hpp"

using pgcpp::access::CreateTupleDesc;
using pgcpp::access::heap_beginscan;
using pgcpp::access::heap_endscan;
using pgcpp::access::heap_form_tuple;
using pgcpp::access::heap_freetuple;
using pgcpp::access::heap_getnext;
using pgcpp::access::heap_insert;
using pgcpp::access::HeapScanDesc;
using pgcpp::access::InitializeRelcache;
using pgcpp::access::Relation;
using pgcpp::access::RelationClose;
using pgcpp::access::RelationCreateStorage;
using pgcpp::access::RelationOpen;
using pgcpp::access::ResetRelcache;
using pgcpp::access::TupleDesc;
using pgcpp::catalog::AttAlign;
using pgcpp::catalog::AttStorage;
using pgcpp::catalog::BootstrapCatalog;
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
using pgcpp::executor::ExecutorEnd;
using pgcpp::executor::ExecutorFinish;
using pgcpp::executor::ExecutorRun;
using pgcpp::executor::ExecutorStart;
using pgcpp::executor::Plan;
using pgcpp::executor::PlanType;
using pgcpp::executor::QueryDesc;
using pgcpp::executor::TupleTableSlot;
using pgcpp::memory::AllocSetContext;
using pgcpp::nodes::makePallocNode;
using pgcpp::optimizer::planner;
using pgcpp::parser::CmdType;
using pgcpp::parser::Const;
using pgcpp::parser::FromExpr;
using pgcpp::parser::kIndexVar;
using pgcpp::parser::Node;
using pgcpp::parser::OpExpr;
using pgcpp::parser::Query;
using pgcpp::parser::RangeTblEntry;
using pgcpp::parser::RangeTblRef;
using pgcpp::parser::RTEKind;
using pgcpp::parser::TargetEntry;
using pgcpp::parser::Var;
using pgcpp::storage::InitBufferPool;
using pgcpp::storage::SetStorageBaseDir;
using pgcpp::storage::ShutdownBufferPool;
using pgcpp::storage::smgrcloseall;
using pgcpp::transaction::BeginTransactionBlock;
using pgcpp::transaction::EndTransactionBlock;
using pgcpp::transaction::HeapTuple;
using pgcpp::transaction::InitializeSnapshotManager;
using pgcpp::transaction::InitializeTransactionSystem;
using pgcpp::transaction::ResetTransactionState;
using pgcpp::types::DatumGetInt32;
using pgcpp::types::Int32GetDatum;
using pgcpp::types::kBoolOid;
using pgcpp::types::kInt4Oid;

namespace {

// Operator OID for int4 = int4 (from bootstrap_catalog.cpp).
constexpr Oid kInt4EqOp = 96;

class UpdateVisibilityTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("update_visibility_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        BootstrapCatalog(catalog_);
        syscache_ = new SysCache();
        SetSysCache(syscache_);

        ResetTransactionState();
        InitializeTransactionSystem();
        InitializeSnapshotManager();
        BeginTransactionBlock();

        test_dir_ = "/tmp/pgcpp_update_vis_test_" + std::to_string(getpid());
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
        InitializeSnapshotManager();

        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    // Commit and start a new transaction so prior writes become visible
    // to a fresh snapshot.
    void CommitAndStartNew() {
        EndTransactionBlock();
        InitializeSnapshotManager();
        BeginTransactionBlock();
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

    // Create a relation with a 2-column int4 schema (a, b).
    Relation CreateTestRelation(Oid relid, const std::string& name) {
        auto* class_row = MakeClassRow(name, relid);
        catalog_->InsertClass(class_row);
        catalog_->InsertAttribute(MakeAttrRow(relid, "a", 1, kInt4Oid, 4, true, AttAlign::kInt));
        catalog_->InsertAttribute(MakeAttrRow(relid, "b", 2, kInt4Oid, 4, true, AttAlign::kInt));
        RelationCreateStorage(relid, false);
        return RelationOpen(relid);
    }

    RangeTblEntry* MakeRTE(Oid relid) {
        auto* rte = makePallocNode<RangeTblEntry>();
        rte->rtekind = RTEKind::kRelation;
        rte->relid = static_cast<int>(relid);
        return rte;
    }

    Const* MakeInt4Const(int32_t value) {
        auto* con = makePallocNode<Const>();
        con->consttype = kInt4Oid;
        con->constvalue = Int32GetDatum(value);
        con->constisnull = false;
        con->constbyval = true;
        con->constlen = 4;
        return con;
    }

    TargetEntry* MakeTargetEntry(Node* expr, int resno, const std::string& resname = "") {
        auto* te = makePallocNode<TargetEntry>();
        te->expr = expr;
        te->resno = resno;
        te->resname = resname;
        return te;
    }

    Var* MakeVar(int varno, int varattno, Oid vartype) {
        auto* var = makePallocNode<Var>();
        var->varno = varno;
        var->varattno = varattno;
        var->vartype = vartype;
        return var;
    }

    // Build an equality qual: Var(varno, varattno) = int4(value).
    Node* MakeEqQual(int varno, int varattno, int32_t value) {
        auto* eq_op = makePallocNode<OpExpr>();
        eq_op->opno = kInt4EqOp;
        eq_op->opresulttype = kBoolOid;
        eq_op->args.push_back(MakeVar(varno, varattno, kInt4Oid));
        eq_op->args.push_back(MakeInt4Const(value));
        return eq_op;
    }

    // Build an INSERT Query for `INSERT INTO target VALUES (a, b)`.
    Query* MakeInsertQuery(Oid relid, int32_t a, int32_t b) {
        auto* rte = MakeRTE(relid);
        auto* query = makePallocNode<Query>();
        query->command_type = CmdType::kInsert;
        query->result_relation = 1;
        query->rtable.push_back(rte);
        query->target_list.push_back(MakeTargetEntry(MakeInt4Const(a), 1, "a"));
        query->target_list.push_back(MakeTargetEntry(MakeInt4Const(b), 2, "b"));
        return query;
    }

    // Build an UPDATE Query for `UPDATE target SET <set_col>=<set_expr>
    // [WHERE <qual>]`. The target list includes ALL columns: the SET
    // column carries set_expr; the other column carries a Var referencing
    // the scan tuple. This mirrors what expandUpdateTargetList produces
    // after the parser fix — without it, non-SET columns would be lost
    // and heap_update would write an all-NULL tuple.
    Query* MakeUpdateQuery(Oid relid, int set_col_idx, Node* set_expr, Node* qual) {
        auto* rte = MakeRTE(relid);
        auto* query = makePallocNode<Query>();
        query->command_type = CmdType::kUpdate;
        query->result_relation = 1;
        query->rtable.push_back(rte);

        // Target list: all columns. SET column uses set_expr; the other
        // uses a Var referencing the scan tuple (varno=1 = target rel).
        Node* a_expr = (set_col_idx == 1) ? set_expr : MakeVar(1, 1, kInt4Oid);
        Node* b_expr = (set_col_idx == 2) ? set_expr : MakeVar(1, 2, kInt4Oid);
        query->target_list.push_back(MakeTargetEntry(a_expr, 1, "a"));
        query->target_list.push_back(MakeTargetEntry(b_expr, 2, "b"));

        // Jointree: FROM target [WHERE qual].
        auto* rtr = makePallocNode<RangeTblRef>();
        rtr->rtindex = 1;
        auto* from_expr = makePallocNode<FromExpr>();
        from_expr->fromlist.push_back(rtr);
        from_expr->quals = qual;
        query->jointree = from_expr;

        return query;
    }

    // Plan and execute an INSERT query through the planner. Drains any
    // RETURNING tuples (none for plain INSERT).
    void RunInsertViaPlanner(Query* query) {
        Plan* plan = planner(query);
        ASSERT_NE(plan, nullptr);
        EXPECT_EQ(plan->type, PlanType::kModifyTable);

        auto* qd = makePallocNode<QueryDesc>();
        qd->query = query;
        qd->plan = plan;
        ExecutorStart(qd);
        TupleTableSlot* slot = nullptr;
        while ((slot = ExecutorRun(qd)) != nullptr) {
            // Drain RETURNING tuples (none expected for plain INSERT).
        }
        ExecutorFinish(qd);
        ExecutorEnd(qd);
    }

    // Plan and execute an UPDATE query through the planner. Returns the
    // number of affected rows (count of slots returned by ExecutorRun).
    // The protocol layer (postgres.cpp) counts these slots to build the
    // "UPDATE N" command tag; before the executor fix, the UPDATE branch
    // used `continue` instead of `return ps_ResultTupleSlot`, so zero
    // slots were returned and the tag was always "UPDATE 0".
    int RunUpdateViaPlanner(Query* query) {
        Plan* plan = planner(query);
        EXPECT_NE(plan, nullptr);
        if (plan == nullptr) {
            return -1;
        }
        EXPECT_EQ(plan->type, PlanType::kModifyTable);

        auto* qd = makePallocNode<QueryDesc>();
        qd->query = query;
        qd->plan = plan;
        ExecutorStart(qd);
        int affected = 0;
        TupleTableSlot* slot = nullptr;
        while ((slot = ExecutorRun(qd)) != nullptr) {
            ++affected;
        }
        ExecutorFinish(qd);
        ExecutorEnd(qd);
        return affected;
    }

    // Scan a relation and return all (a, b) rows. Caller must have called
    // CommitAndStartNew() first so prior writes are visible.
    std::vector<std::pair<int, int>> ScanAllRows(Oid relid) {
        Relation rel = RelationOpen(relid);
        HeapScanDesc scan = heap_beginscan(rel, nullptr);
        std::vector<std::pair<int, int>> rows;
        HeapTuple tup = nullptr;
        while ((tup = heap_getnext(scan)) != nullptr) {
            TupleTableSlot* slot = TupleTableSlot::Make(rel->rd_att);
            slot->StoreTuple(tup, false);
            int a = slot->tts_isnull[0] ? 0 : DatumGetInt32(slot->tts_values[0]);
            int b = slot->tts_isnull[1] ? 0 : DatumGetInt32(slot->tts_values[1]);
            rows.emplace_back(a, b);
        }
        heap_endscan(scan);
        RelationClose(rel);
        return rows;
    }

    // Collect rows into a map for value-based lookups: column a → column b.
    std::map<int, int> RowsToMap(const std::vector<std::pair<int, int>>& rows) {
        std::map<int, int> result;
        for (const auto& r : rows) {
            result[r.first] = r.second;
        }
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
};

// ===========================================================================
// 1. InsertSingleRowThenUpdateThenSelect
//
// Core regression for "UPDATE then SELECT sees updated value". Before the
// fix, UPDATE wrote an all-NULL tuple (because ModifyTable's targetlist
// was empty) and SELECT returned NULL. After the fix, the SET expression
// is applied and the non-SET column is preserved.
// ===========================================================================
TEST_F(UpdateVisibilityTest, InsertSingleRowThenUpdateThenSelect) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "target");
    RelationClose(rel);

    // INSERT (1, 10) — through the planner (exercises the INSERT fix).
    RunInsertViaPlanner(MakeInsertQuery(relid, 1, 10));
    CommitAndStartNew();

    // UPDATE target SET b = 99 WHERE a = 1 — through the planner.
    int affected = RunUpdateViaPlanner(MakeUpdateQuery(relid, /*set_col_idx=*/2, MakeInt4Const(99),
                                                       /*qual=*/MakeEqQual(1, 1, 1)));
    EXPECT_EQ(affected, 1) << "UPDATE WHERE a=1 should affect 1 row";
    CommitAndStartNew();

    // SELECT — verify (1, 99): b was updated, a was preserved.
    auto rows = ScanAllRows(relid);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].first, 1) << "non-SET column 'a' must be preserved";
    EXPECT_EQ(rows[0].second, 99) << "SET column 'b' must reflect new value";
}

// ===========================================================================
// 2. InsertMultipleRowsUpdatePartialMatch
//
// INSERT 3 rows, UPDATE WHERE matches exactly one row. Verifies:
//   - affected row count is 1 (not 0, not 3)
//   - only the matched row's SET column changed
//   - non-SET columns of all rows are preserved
//   - unmatched rows are untouched
// ===========================================================================
TEST_F(UpdateVisibilityTest, InsertMultipleRowsUpdatePartialMatch) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "target");
    RelationClose(rel);

    // INSERT 3 rows: (1,10), (2,20), (3,30).
    RunInsertViaPlanner(MakeInsertQuery(relid, 1, 10));
    RunInsertViaPlanner(MakeInsertQuery(relid, 2, 20));
    RunInsertViaPlanner(MakeInsertQuery(relid, 3, 30));
    CommitAndStartNew();

    // UPDATE target SET b = 0 WHERE a = 2 — should affect exactly 1 row.
    int affected = RunUpdateViaPlanner(MakeUpdateQuery(relid, /*set_col_idx=*/2, MakeInt4Const(0),
                                                       /*qual=*/MakeEqQual(1, 1, 2)));
    EXPECT_EQ(affected, 1) << "UPDATE WHERE a=2 should affect 1 row";
    CommitAndStartNew();

    // SELECT — verify only row 2 changed, others untouched.
    auto rows = ScanAllRows(relid);
    ASSERT_EQ(rows.size(), 3u);
    auto row_map = RowsToMap(rows);
    EXPECT_EQ(row_map[1], 10) << "row 1 must be unchanged";
    EXPECT_EQ(row_map[2], 0) << "row 2 must have b=0 after UPDATE";
    EXPECT_EQ(row_map[3], 30) << "row 3 must be unchanged";
}

// ===========================================================================
// 3. UpdateAllRowsNoWhere
//
// UPDATE with no WHERE clause should update every row. Verifies:
//   - affected count equals total row count
//   - all rows have the new SET value
//   - non-SET columns are preserved across all rows
// ===========================================================================
TEST_F(UpdateVisibilityTest, UpdateAllRowsNoWhere) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "target");
    RelationClose(rel);

    // INSERT 3 rows.
    RunInsertViaPlanner(MakeInsertQuery(relid, 1, 10));
    RunInsertViaPlanner(MakeInsertQuery(relid, 2, 20));
    RunInsertViaPlanner(MakeInsertQuery(relid, 3, 30));
    CommitAndStartNew();

    // UPDATE target SET b = 0 (no WHERE) — should affect all 3 rows.
    int affected = RunUpdateViaPlanner(MakeUpdateQuery(relid, /*set_col_idx=*/2, MakeInt4Const(0),
                                                       /*qual=*/nullptr));
    EXPECT_EQ(affected, 3) << "UPDATE with no WHERE should affect all 3 rows";
    CommitAndStartNew();

    // SELECT — verify all rows have b=0 and a is preserved.
    auto rows = ScanAllRows(relid);
    ASSERT_EQ(rows.size(), 3u);
    auto row_map = RowsToMap(rows);
    EXPECT_EQ(row_map[1], 0) << "row 1 b must be 0";
    EXPECT_EQ(row_map[2], 0) << "row 2 b must be 0";
    EXPECT_EQ(row_map[3], 0) << "row 3 b must be 0";
    // Verify 'a' values survived (non-SET column preservation).
    EXPECT_TRUE(row_map.count(1));
    EXPECT_TRUE(row_map.count(2));
    EXPECT_TRUE(row_map.count(3));
}

// ===========================================================================
// 4. UpdateWhereNoMatchReturnsZero
//
// UPDATE WHERE matches no rows should affect 0 rows and leave all data
// unchanged. Before the executor fix, the UPDATE branch used `continue`
// (never returned a slot), so the count was always 0 — even when rows
// matched. This test confirms that 0 is now the CORRECT count (no rows
// matched) rather than a symptom of the executor bug.
// ===========================================================================
TEST_F(UpdateVisibilityTest, UpdateWhereNoMatchReturnsZero) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "target");
    RelationClose(rel);

    // INSERT 3 rows: (1,10), (2,20), (3,30).
    RunInsertViaPlanner(MakeInsertQuery(relid, 1, 10));
    RunInsertViaPlanner(MakeInsertQuery(relid, 2, 20));
    RunInsertViaPlanner(MakeInsertQuery(relid, 3, 30));
    CommitAndStartNew();

    // UPDATE target SET b = 0 WHERE a = 999 — no row matches.
    int affected = RunUpdateViaPlanner(MakeUpdateQuery(relid, /*set_col_idx=*/2, MakeInt4Const(0),
                                                       /*qual=*/MakeEqQual(1, 1, 999)));
    EXPECT_EQ(affected, 0) << "UPDATE WHERE a=999 should affect 0 rows";
    CommitAndStartNew();

    // SELECT — verify all data is unchanged.
    auto rows = ScanAllRows(relid);
    ASSERT_EQ(rows.size(), 3u) << "no rows should have been deleted or corrupted";
    auto row_map = RowsToMap(rows);
    EXPECT_EQ(row_map[1], 10) << "row 1 must be unchanged";
    EXPECT_EQ(row_map[2], 20) << "row 2 must be unchanged";
    EXPECT_EQ(row_map[3], 30) << "row 3 must be unchanged";
}

}  // namespace

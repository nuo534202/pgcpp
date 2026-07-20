// insert_select_visibility_test.cpp — P0 regression tests for INSERT→SELECT
// visibility through the planner.
//
// Verifies the fix in src/optimizer/plan/planner.cpp: for INSERT without
// RETURNING, ModifyTable's targetlist is now populated with Var nodes
// (varno = kIndexVar) referencing the child plan's output columns. Before
// the fix, the targetlist was empty, so ExecProject left the result slot
// all-NULL and heap_form_tuple wrote NULL tuples — SELECT * FROM t then
// returned NULL instead of the inserted data.
//
// Unlike node_modify_table_test.cpp (which manually constructs ModifyTable
// plans with a correct targetlist, bypassing the planner), these tests
// build a Query tree and call pgcpp::optimizer::planner() so the planner's
// ModifyTable-targetlist construction is exercised end-to-end.
//
// Coverage:
//   1. INSERT single row → SELECT returns the inserted values (not NULL)
//   2. INSERT multiple rows (separate statements) → SELECT returns all rows
//   3. INSERT → UPDATE (manual plan) → SELECT returns updated values
//   4. Planner produces ModifyTable with non-empty targetlist of Vars
//      (direct regression test for the bug)

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <set>
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
#include "parser/analyze.hpp"
#include "parser/parsenodes.hpp"
#include "parser/parser.hpp"
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
using pgcpp::executor::ModifyTable;
using pgcpp::executor::Plan;
using pgcpp::executor::PlanType;
using pgcpp::executor::QueryDesc;
using pgcpp::executor::SeqScan;
using pgcpp::executor::TupleTableSlot;
using pgcpp::memory::AllocSetContext;
using pgcpp::nodes::makePallocNode;
using pgcpp::nodes::NodeTag;
using pgcpp::optimizer::planner;
using pgcpp::parser::CmdType;
using pgcpp::parser::Const;
using pgcpp::parser::kIndexVar;
using pgcpp::parser::Node;
using pgcpp::parser::parse_analyze;
using pgcpp::parser::Query;
using pgcpp::parser::RangeTblEntry;
using pgcpp::parser::raw_parser;
using pgcpp::parser::RawStmt;
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
using pgcpp::types::kInt4Oid;

namespace {

// Operator OID for int4 = int4 (from bootstrap_catalog.cpp).
constexpr Oid kInt4EqOp = 96;

class InsertSelectVisibilityTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("insert_visibility_test_context");
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

        test_dir_ = "/tmp/pgcpp_insert_vis_test_" + std::to_string(getpid());
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

    // Commit and start a new transaction so inserted tuples become visible
    // to a fresh snapshot.
    void CommitAndStartNew() {
        EndTransactionBlock();
        InitializeSnapshotManager();
        BeginTransactionBlock();
    }

    // Build a pg_class row.
    FormData_pg_class* MakeClassRow(const std::string& name, Oid oid) {
        auto* row = makePallocNode<FormData_pg_class>();
        row->oid = oid;
        row->relname = name;
        row->relfilenode = oid;
        row->relkind = RelKind::kRelation;
        row->relpersistence = RelPersistence::kPermanent;
        return row;
    }

    // Build a pg_attribute row.
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

    // Build a RangeTblEntry for a relation.
    RangeTblEntry* MakeRTE(Oid relid) {
        auto* rte = makePallocNode<RangeTblEntry>();
        rte->rtekind = RTEKind::kRelation;
        rte->relid = static_cast<int>(relid);
        return rte;
    }

    // Build a Const node for int4.
    Const* MakeInt4Const(int32_t value) {
        auto* con = makePallocNode<Const>();
        con->consttype = kInt4Oid;
        con->constvalue = Int32GetDatum(value);
        con->constisnull = false;
        con->constbyval = true;
        con->constlen = 4;
        return con;
    }

    // Build a TargetEntry.
    TargetEntry* MakeTargetEntry(Node* expr, int resno, const std::string& resname = "") {
        auto* te = makePallocNode<TargetEntry>();
        te->expr = expr;
        te->resno = resno;
        te->resname = resname;
        return te;
    }

    // Build a Var node.
    Var* MakeVar(int varno, int varattno, Oid vartype) {
        auto* var = makePallocNode<Var>();
        var->varno = varno;
        var->varattno = varattno;
        var->vartype = vartype;
        return var;
    }

    // Build an INSERT Query for `INSERT INTO target VALUES (a, b)`.
    // The query has no jointree (no FROM clause); the planner produces a
    // Result child plan wrapped in ModifyTable.
    Query* MakeInsertQuery(Oid relid, int32_t a, int32_t b) {
        auto* rte = MakeRTE(relid);
        auto* query = makePallocNode<Query>();
        query->command_type = CmdType::kInsert;
        query->result_relation = 1;  // target is RT entry 1
        query->rtable.push_back(rte);
        // No jointree: INSERT ... VALUES has no FROM clause.
        // Target list carries the VALUES constants in target-table column
        // order, matching how transformInsertStmt builds the list.
        query->target_list.push_back(MakeTargetEntry(MakeInt4Const(a), 1, "a"));
        query->target_list.push_back(MakeTargetEntry(MakeInt4Const(b), 2, "b"));
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

    // Scan a relation and return all (a, b) rows. Caller must have called
    // CommitAndStartNew() first so inserted rows are visible.
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
// 1. InsertSingleRow_ThroughPlanner_SelectReturnsData
//
// The core P0 regression: before the fix, INSERT wrote a NULL tuple because
// ModifyTable's targetlist was empty. After commit, SELECT * returned NULL.
// This test verifies the inserted values survive a planner-driven INSERT +
// commit + scan cycle.
// ===========================================================================
TEST_F(InsertSelectVisibilityTest, InsertSingleRow_ThroughPlanner_SelectReturnsData) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "target");
    RelationClose(rel);

    // INSERT INTO target VALUES (42, 99) — through the planner.
    Query* insert_query = MakeInsertQuery(relid, 42, 99);
    RunInsertViaPlanner(insert_query);

    // Commit so the inserted row is visible to a fresh snapshot.
    CommitAndStartNew();

    auto rows = ScanAllRows(relid);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].first, 42);
    EXPECT_EQ(rows[0].second, 99);
}

// ===========================================================================
// 2. InsertMultipleRows_ThroughPlanner_AllVisible
//
// Each INSERT statement goes through the planner independently, exercising
// the ModifyTable-targetlist fix for every row. After commit, all rows
// must be visible with their correct values.
// ===========================================================================
TEST_F(InsertSelectVisibilityTest, InsertMultipleRows_ThroughPlanner_AllVisible) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "target");
    RelationClose(rel);

    // Three separate INSERT statements, each planned independently.
    RunInsertViaPlanner(MakeInsertQuery(relid, 1, 10));
    RunInsertViaPlanner(MakeInsertQuery(relid, 2, 20));
    RunInsertViaPlanner(MakeInsertQuery(relid, 3, 30));

    CommitAndStartNew();

    auto rows = ScanAllRows(relid);
    ASSERT_EQ(rows.size(), 3u);

    std::set<int> a_values;
    std::set<int> b_values;
    for (const auto& r : rows) {
        a_values.insert(r.first);
        b_values.insert(r.second);
    }
    EXPECT_EQ(a_values.size(), 3u);
    EXPECT_TRUE(a_values.count(1));
    EXPECT_TRUE(a_values.count(2));
    EXPECT_TRUE(a_values.count(3));
    EXPECT_TRUE(b_values.count(10));
    EXPECT_TRUE(b_values.count(20));
    EXPECT_TRUE(b_values.count(30));
}

// ===========================================================================
// 3. InsertThenUpdateThenSelect
//
// INSERT a row through the planner (the fix path), commit, UPDATE the row
// via a manually-constructed ModifyTable plan (UPDATE-through-planner is a
// separate Task 2 — the child targetlist only carries SET columns today),
// commit, then scan to verify the updated value. This confirms the inserted
// row is durable and can be mutated by a subsequent UPDATE.
// ===========================================================================
TEST_F(InsertSelectVisibilityTest, InsertThenUpdateThenSelect) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "target");
    RelationClose(rel);

    // Step 1: INSERT (1, 10) through the planner.
    RunInsertViaPlanner(MakeInsertQuery(relid, 1, 10));
    CommitAndStartNew();

    // Verify the insert is durable before updating.
    auto rows_after_insert = ScanAllRows(relid);
    ASSERT_EQ(rows_after_insert.size(), 1u);
    EXPECT_EQ(rows_after_insert[0].first, 1);
    EXPECT_EQ(rows_after_insert[0].second, 10);

    // Step 2: UPDATE target SET b = 99 WHERE a = 1 via a manually-constructed
    // plan. The SeqScan child projects (a, 99) — SET b = 99 — and filters on
    // a = 1. ModifyTable's targetlist references the child's output columns
    // (matching the existing UpdateWithCondition test in
    // node_modify_table_test.cpp, which is known to work).
    auto* seqplan = makePallocNode<SeqScan>();
    seqplan->scanrelid = 1;
    seqplan->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    seqplan->targetlist.push_back(MakeTargetEntry(MakeInt4Const(99), 2, "b"));
    // qual: a = 1
    auto* eq_op = makePallocNode<pgcpp::parser::OpExpr>();
    eq_op->opno = kInt4EqOp;
    eq_op->opresulttype = pgcpp::types::kBoolOid;
    eq_op->args.push_back(MakeVar(1, 1, kInt4Oid));
    eq_op->args.push_back(MakeInt4Const(1));
    seqplan->qual = eq_op;

    auto* mt = makePallocNode<ModifyTable>();
    mt->operation = CmdType::kUpdate;
    mt->resultRelid = 1;
    mt->lefttree = seqplan;
    mt->targetlist.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    mt->targetlist.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    auto* update_query = makePallocNode<Query>();
    update_query->command_type = CmdType::kUpdate;
    update_query->result_relation = 1;
    update_query->rtable.push_back(MakeRTE(relid));
    auto* update_qd = makePallocNode<QueryDesc>();
    update_qd->query = update_query;
    update_qd->plan = mt;
    ExecutorStart(update_qd);
    TupleTableSlot* slot = nullptr;
    while ((slot = ExecutorRun(update_qd)) != nullptr) {
    }
    ExecutorFinish(update_qd);
    ExecutorEnd(update_qd);

    CommitAndStartNew();

    // Step 3: SELECT — verify the row now has b = 99 (and a unchanged).
    auto rows = ScanAllRows(relid);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].first, 1);
    EXPECT_EQ(rows[0].second, 99);
}

// ===========================================================================
// 4. Planner_ProducesModifyTableWithVarTargetlist
//
// Direct regression test for the root cause: the planner must populate
// ModifyTable's targetlist with Var nodes (varno = kIndexVar) referencing
// the child plan's output columns for INSERT without RETURNING. Before the
// fix, the targetlist was empty, which caused ExecProject to leave the
// result slot all-NULL.
// ===========================================================================
TEST_F(InsertSelectVisibilityTest, Planner_ProducesModifyTableWithVarTargetlist) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "target");
    RelationClose(rel);

    Query* query = MakeInsertQuery(relid, 7, 77);
    Plan* plan = planner(query);

    ASSERT_NE(plan, nullptr);
    ASSERT_EQ(plan->type, PlanType::kModifyTable);
    auto* mt = static_cast<ModifyTable*>(plan);
    EXPECT_EQ(mt->operation, CmdType::kInsert);
    EXPECT_EQ(mt->resultRelid, 1);

    // The fix: ModifyTable's targetlist must be non-empty and contain Var
    // nodes with varno = kIndexVar referencing the child plan's output.
    ASSERT_EQ(mt->targetlist.size(), 2u)
        << "ModifyTable targetlist must be populated for INSERT without RETURNING";
    for (size_t i = 0; i < mt->targetlist.size(); i++) {
        auto* te = mt->targetlist[i];
        ASSERT_NE(te->expr, nullptr);
        EXPECT_EQ(te->expr->GetTag(), pgcpp::nodes::NodeTag::kVar)
            << "ModifyTable targetlist entry " << i << " must be a Var";
        auto* var = static_cast<Var*>(te->expr);
        EXPECT_EQ(var->varno, kIndexVar) << "Var must reference the child plan via kIndexVar";
        EXPECT_EQ(var->varattno, static_cast<int>(i + 1))
            << "Var must reference child column " << (i + 1);
    }

    // Child plan (Result) must have the original Const targetlist.
    ASSERT_NE(mt->lefttree, nullptr);
    EXPECT_EQ(mt->lefttree->type, pgcpp::executor::PlanType::kResult);
    EXPECT_EQ(mt->lefttree->targetlist.size(), 2u);
}

// ===========================================================================
// 5. MultiRowInsert_ThroughPlanner_AllRowsVisible
//
// Regression for the bug where `INSERT INTO t VALUES (1,10),(2,20),(3,30)`
// (a single multi-row INSERT statement) failed with
// "unsupported expression type in ExecEvalExpr". The multi-row path in
// transformInsertStmt (analyze.cpp) coerced expressions WITHOUT first
// calling transformExpr, so raw AConst nodes from the parser remained in
// the VALUES RTE's values_lists. ValuesScan's ExecEvalExpr has no case
// for kAConst and threw. The fix calls transformExpr on each expression
// before coercion, matching the single-row INSERT path.
//
// This test parses and analyzes real SQL `INSERT INTO ... VALUES (...),(...)`,
// runs it through the planner+executor, commits, and scans to verify all
// rows are visible with the correct values.
// ===========================================================================
TEST_F(InsertSelectVisibilityTest, MultiRowInsert_ThroughPlanner_AllRowsVisible) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "target");
    RelationClose(rel);

    // Parse + analyze `INSERT INTO target VALUES (1,10),(2,20),(3,30)`.
    // The parser resolves "target" against the current catalog; the
    // relation was just registered with name "target".
    const char* sql = "INSERT INTO target VALUES (1, 10), (2, 20), (3, 30)";
    auto stmts = raw_parser(sql);
    ASSERT_FALSE(stmts.empty());
    auto queries = parse_analyze(stmts, sql);
    ASSERT_EQ(queries.size(), 1u);
    Query* insert_query = queries[0];
    ASSERT_NE(insert_query, nullptr);
    EXPECT_EQ(insert_query->command_type, CmdType::kInsert);

    // The multi-row path adds a VALUES RTE (kind = kValues) to rtable.
    // Verify the RTE exists and its values_lists contain Const nodes
    // (the fix), not AConst nodes (the bug).
    bool found_values_rte = false;
    for (Node* rte_node : insert_query->rtable) {
        if (rte_node->GetTag() != NodeTag::kRangeTblEntry)
            continue;
        auto* rte = static_cast<RangeTblEntry*>(rte_node);
        if (rte->rtekind == RTEKind::kValues && !rte->values_lists.empty()) {
            found_values_rte = true;
            ASSERT_EQ(rte->values_lists.size(), 3u);
            for (size_t r = 0; r < rte->values_lists.size(); ++r) {
                const auto& row = rte->values_lists[r];
                ASSERT_EQ(row.size(), 2u);
                for (size_t c = 0; c < row.size(); ++c) {
                    EXPECT_EQ(row[c]->GetTag(), NodeTag::kConst)
                        << "VALUES RTE row " << r << " col " << c
                        << " must be a Const after transformExpr (not AConst)";
                }
            }
            break;
        }
    }
    EXPECT_TRUE(found_values_rte) << "Multi-row INSERT must produce a VALUES RTE in rtable";

    // Plan + execute.
    RunInsertViaPlanner(insert_query);

    // Commit so inserted rows are visible to a fresh snapshot.
    CommitAndStartNew();

    // Scan and verify all 3 rows with correct values.
    auto rows = ScanAllRows(relid);
    ASSERT_EQ(rows.size(), 3u);

    std::set<int> a_values;
    std::set<int> b_values;
    for (const auto& r : rows) {
        a_values.insert(r.first);
        b_values.insert(r.second);
    }
    EXPECT_EQ(a_values.size(), 3u);
    EXPECT_TRUE(a_values.count(1));
    EXPECT_TRUE(a_values.count(2));
    EXPECT_TRUE(a_values.count(3));
    EXPECT_TRUE(b_values.count(10));
    EXPECT_TRUE(b_values.count(20));
    EXPECT_TRUE(b_values.count(30));
}

// ===========================================================================
// 6. MultiRowInsert_WithNull_AllRowsVisible
//
// Regression combining both fixes: `INSERT INTO t VALUES (1,NULL),(NULL,2)`
// exercises (a) the multi-row path that must call transformExpr on each
// row's expressions, and (b) the NULL-handling fix in coerce_type that
// must not produce a CoerceViaIO for a NULL Const of kUnknownOid.
//
// ScanAllRows returns (0, ...) or (..., 0) for NULL columns — we use
// tts_isnull directly to distinguish NULL from a real 0, so this test
// verifies NULL is preserved in the heap tuple.
// ===========================================================================
TEST_F(InsertSelectVisibilityTest, MultiRowInsert_WithNull_AllRowsVisible) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "target");
    RelationClose(rel);

    // Parse + analyze `INSERT INTO target VALUES (1, NULL), (NULL, 2)`.
    const char* sql = "INSERT INTO target VALUES (1, NULL), (NULL, 2)";
    auto stmts = raw_parser(sql);
    ASSERT_FALSE(stmts.empty());
    auto queries = parse_analyze(stmts, sql);
    ASSERT_EQ(queries.size(), 1u);
    Query* insert_query = queries[0];

    // Verify the VALUES RTE contains Const nodes (including NULL Consts).
    for (Node* rte_node : insert_query->rtable) {
        if (rte_node->GetTag() != NodeTag::kRangeTblEntry)
            continue;
        auto* rte = static_cast<RangeTblEntry*>(rte_node);
        if (rte->rtekind == RTEKind::kValues && !rte->values_lists.empty()) {
            ASSERT_EQ(rte->values_lists.size(), 2u);
            for (size_t r = 0; r < rte->values_lists.size(); ++r) {
                const auto& row = rte->values_lists[r];
                ASSERT_EQ(row.size(), 2u);
                for (size_t c = 0; c < row.size(); ++c) {
                    EXPECT_EQ(row[c]->GetTag(), NodeTag::kConst)
                        << "VALUES RTE row " << r << " col " << c << " must be a Const";
                    // NULL literals must NOT be wrapped in CoerceViaIO.
                    auto* con = static_cast<Const*>(row[c]);
                    if (con->constisnull) {
                        EXPECT_EQ(con->consttype, pgcpp::types::kInt4Oid)
                            << "NULL literal must carry the target column type (int4)";
                    }
                }
            }
            break;
        }
    }

    // Plan + execute — must not throw "unsupported expression type".
    RunInsertViaPlanner(insert_query);

    CommitAndStartNew();

    // Scan and verify both rows: (1, NULL) and (NULL, 2).
    Relation scan_rel = RelationOpen(relid);
    HeapScanDesc scan = heap_beginscan(scan_rel, nullptr);
    std::vector<std::pair<int, bool>> row_a;  // (value, is_null)
    std::vector<std::pair<int, bool>> row_b;
    HeapTuple tup = nullptr;
    while ((tup = heap_getnext(scan)) != nullptr) {
        TupleTableSlot* slot = TupleTableSlot::Make(scan_rel->rd_att);
        slot->StoreTuple(tup, false);
        int a = slot->tts_isnull[0] ? 0 : DatumGetInt32(slot->tts_values[0]);
        int b = slot->tts_isnull[1] ? 0 : DatumGetInt32(slot->tts_values[1]);
        row_a.emplace_back(a, slot->tts_isnull[0]);
        row_b.emplace_back(b, slot->tts_isnull[1]);
    }
    heap_endscan(scan);
    RelationClose(scan_rel);

    ASSERT_EQ(row_a.size(), 2u);
    // One row has a=1, b=NULL; the other has a=NULL, b=2.
    int null_a_count = 0;
    int null_b_count = 0;
    int a_one_count = 0;
    int b_two_count = 0;
    for (size_t i = 0; i < row_a.size(); ++i) {
        if (row_a[i].second)
            ++null_a_count;
        if (row_b[i].second)
            ++null_b_count;
        if (!row_a[i].second && row_a[i].first == 1)
            ++a_one_count;
        if (!row_b[i].second && row_b[i].first == 2)
            ++b_two_count;
    }
    EXPECT_EQ(null_a_count, 1) << "Exactly one row must have NULL in column a";
    EXPECT_EQ(null_b_count, 1) << "Exactly one row must have NULL in column b";
    EXPECT_EQ(a_one_count, 1) << "Exactly one row must have a=1";
    EXPECT_EQ(b_two_count, 1) << "Exactly one row must have b=2";
}

// ===========================================================================
// 7. SingleRowInsert_WithNull_PreservesNull
//
// Direct regression for the NULL-handling bug in coerce_type. Single-row
// `INSERT INTO t VALUES (1, NULL)` goes through transformTargetList →
// transformExpr (so AConst → Const), then coerce_type for the NULL Const
// of kUnknownOid. Before the fix, coerce_type built a CoerceViaIO that
// ExecEvalExpr could not evaluate. After the fix, the NULL Const keeps
// the target type metadata and ExecEvalExpr evaluates it as a NULL Datum.
// ===========================================================================
TEST_F(InsertSelectVisibilityTest, SingleRowInsert_WithNull_PreservesNull) {
    Oid relid = kFirstNormalObjectId;
    Relation rel = CreateTestRelation(relid, "target");
    RelationClose(rel);

    const char* sql = "INSERT INTO target VALUES (42, NULL)";
    auto stmts = raw_parser(sql);
    ASSERT_FALSE(stmts.empty());
    auto queries = parse_analyze(stmts, sql);
    ASSERT_EQ(queries.size(), 1u);
    Query* insert_query = queries[0];

    // Plan + execute — must not throw "unsupported expression type".
    RunInsertViaPlanner(insert_query);

    CommitAndStartNew();

    // Scan: expect one row with a=42, b=NULL.
    Relation scan_rel = RelationOpen(relid);
    HeapScanDesc scan = heap_beginscan(scan_rel, nullptr);
    HeapTuple tup = heap_getnext(scan);
    ASSERT_NE(tup, nullptr);
    TupleTableSlot* slot = TupleTableSlot::Make(scan_rel->rd_att);
    slot->StoreTuple(tup, false);
    EXPECT_FALSE(slot->tts_isnull[0]);
    EXPECT_EQ(DatumGetInt32(slot->tts_values[0]), 42);
    EXPECT_TRUE(slot->tts_isnull[1]) << "b must be NULL";
    // Ensure only one row.
    EXPECT_EQ(heap_getnext(scan), nullptr);
    heap_endscan(scan);
    RelationClose(scan_rel);
}

}  // namespace

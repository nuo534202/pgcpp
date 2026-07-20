// cte_test.cpp — End-to-end CTE (WITH clause) execution tests (Task 12).
//
// Verifies that the planner generates a SubqueryScan plan for a CTE
// reference (a subquery RTE produced by transformWithClause +
// transformFromClause) and that the executor produces correct tuples.
//
// These tests exercise the fix in src/optimizer/plan/subplanner.cpp:
// before the fix, the planner emitted a SeqScan for a subquery RTE, and
// the SeqScan executor failed with "relation not found" because the RTE
// has no relid. With the fix, the planner recursively plans the CTE's
// subquery and wraps it in a SubqueryScan.
//
// Coverage (SubTask 12.5):
//   1. Basic non-recursive CTE: WITH t AS (SELECT 1 AS x) SELECT * FROM t
//   2. CTE with multiple columns: WITH t AS (SELECT 1 AS a, 2 AS b) SELECT * FROM t
//   3. CTE referenced multiple times: WITH t AS (SELECT 1 AS x)
//      SELECT x, x+1, x+2 FROM t
//   4. CTE with WHERE clause in outer query: WITH t AS (SELECT 5 AS x)
//      SELECT * FROM t WHERE x > 3  (and the rejecting variant x > 10)
//
// All tests build the analyzed Query tree by hand (no real relation
// needed — the CTE body is a SELECT-from-constants). A WHERE clause
// *inside* the CTE body that scans a real relation is not covered here
// because it requires the planner's set_plan_references pass to flatten
// the CTE body's range table into the parent's, which is beyond Task 12.

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "access/rel.hpp"
#include "catalog/bootstrap_catalog.hpp"
#include "catalog/catalog.hpp"
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
#include "transaction/snapshot.hpp"
#include "transaction/transam.hpp"
#include "transaction/xact.hpp"
#include "types/datum.hpp"

using pgcpp::access::InitializeRelcache;
using pgcpp::access::ResetRelcache;
using pgcpp::catalog::BootstrapCatalog;
using pgcpp::catalog::Catalog;
using pgcpp::catalog::Oid;
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
using pgcpp::transaction::InitializeSnapshotManager;
using pgcpp::transaction::InitializeTransactionSystem;
using pgcpp::transaction::ResetTransactionState;
using pgcpp::types::DatumGetInt32;
using pgcpp::types::Int32GetDatum;
using pgcpp::types::kBoolOid;
using pgcpp::types::kInt4Oid;

namespace {

// Operator OIDs (from bootstrap_catalog.cpp).
constexpr Oid kInt4GtOp = 521;    // int4 > int4
constexpr Oid kInt4PlusOp = 551;  // int4 + int4

class CteExecutorTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("cte_test_context");
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

        test_dir_ = "/tmp/pgcpp_cte_test_" + std::to_string(getpid());
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

    // --- Query-tree builders ------------------------------------------------

    // Build a RangeTblEntry for a subquery (used for CTE references).
    RangeTblEntry* MakeSubqueryRte(Query* subquery) {
        auto* rte = makePallocNode<RangeTblEntry>();
        rte->rtekind = RTEKind::kSubquery;
        rte->subquery = subquery;
        return rte;
    }

    // Build a Var node.
    Var* MakeVar(int varno, int varattno, Oid vartype) {
        auto* var = makePallocNode<Var>();
        var->varno = varno;
        var->varattno = varattno;
        var->vartype = vartype;
        return var;
    }

    // Build an int4 Const.
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

    // Build an OpExpr (e.g., a > b).
    OpExpr* MakeOpExpr(Oid opno, Oid resulttype, Node* left, Node* right) {
        auto* op = makePallocNode<OpExpr>();
        op->opno = opno;
        op->opresulttype = resulttype;
        op->args.push_back(left);
        op->args.push_back(right);
        return op;
    }

    // Build a RangeTblRef with the given 1-based rtindex.
    RangeTblRef* MakeRangeTblRef(int rtindex) {
        auto* ref = makePallocNode<RangeTblRef>();
        ref->rtindex = rtindex;
        return ref;
    }

    // Build a FromExpr with one FROM item and an optional qual.
    FromExpr* MakeFromExpr(Node* from_item, Node* qual = nullptr) {
        auto* from = makePallocNode<FromExpr>();
        from->fromlist.push_back(from_item);
        from->quals = qual;
        return from;
    }

    // Plan a SELECT Query through the planner and execute it, draining
    // all result rows. Returns the collected slots' first-column int32
    // values for single-column results. For multi-column results, use
    // RunSelectAndGetSlots() instead.
    std::vector<int32_t> RunSelectSingleColumn(Query* query) {
        Plan* plan = planner(query);
        EXPECT_NE(plan, nullptr);
        if (plan == nullptr)
            return {};
        // The fix in subplanner.cpp should make this a SubqueryScan for
        // CTE references; SeqScan would fail with "relation not found".
        EXPECT_EQ(plan->type, PlanType::kSubqueryScan)
            << "expected SubqueryScan plan for CTE reference";

        auto* qd = makePallocNode<QueryDesc>();
        qd->query = query;
        qd->plan = plan;

        ExecutorStart(qd);
        std::vector<int32_t> results;
        TupleTableSlot* slot = nullptr;
        while ((slot = ExecutorRun(qd)) != nullptr) {
            results.push_back(DatumGetInt32(slot->tts_values[0]));
        }
        ExecutorFinish(qd);
        ExecutorEnd(qd);
        return results;
    }

    // Plan and execute a SELECT Query, returning all result slots (each
    // slot's tts_values can be read by the caller).
    void RunSelectAndGetSlots(Query* query, std::vector<TupleTableSlot*>* slots) {
        Plan* plan = planner(query);
        ASSERT_NE(plan, nullptr);
        EXPECT_EQ(plan->type, PlanType::kSubqueryScan)
            << "expected SubqueryScan plan for CTE reference";

        auto* qd = makePallocNode<QueryDesc>();
        qd->query = query;
        qd->plan = plan;

        ExecutorStart(qd);
        TupleTableSlot* slot = nullptr;
        while ((slot = ExecutorRun(qd)) != nullptr) {
            slots->push_back(slot);
        }
        ExecutorFinish(qd);
        ExecutorEnd(qd);
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
// SubTask 12.5.1 — Basic non-recursive CTE
//
//   WITH t AS (SELECT 1 AS x) SELECT * FROM t;
//
// The CTE body is a SELECT-from-constants. The outer query references
// the CTE via a subquery RTE in its FROM clause.
// ===========================================================================

TEST_F(CteExecutorTest, BasicNonRecursiveCte) {
    // Build the CTE body: SELECT 1 AS x  (no FROM clause).
    auto* cte_query = makePallocNode<Query>();
    cte_query->command_type = CmdType::kSelect;
    cte_query->can_set_tag = true;
    cte_query->target_list.push_back(MakeTargetEntry(MakeInt4Const(1), 1, "x"));
    // No jointree (no FROM clause).

    // Build the outer query: SELECT x FROM t  where t is the CTE.
    auto* outer = makePallocNode<Query>();
    outer->command_type = CmdType::kSelect;
    outer->can_set_tag = true;
    // Range table: a single subquery RTE pointing at the CTE's Query.
    outer->rtable.push_back(MakeSubqueryRte(cte_query));
    // FROM clause: RangeTblRef{rtindex=1}.
    outer->jointree = MakeFromExpr(MakeRangeTblRef(1));
    // Target list: SELECT x → Var(varno=1, varattno=1).
    outer->target_list.push_back(
        MakeTargetEntry(MakeVar(/*varno=*/1, /*varattno=*/1, kInt4Oid), 1, "x"));

    auto results = RunSelectSingleColumn(outer);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0], 1);
}

// ===========================================================================
// SubTask 12.5.2 — CTE with multiple columns
//
//   WITH t AS (SELECT 1 AS a, 2 AS b) SELECT * FROM t;
// ===========================================================================

TEST_F(CteExecutorTest, CteWithMultipleColumns) {
    // CTE body: SELECT 1 AS a, 2 AS b.
    auto* cte_query = makePallocNode<Query>();
    cte_query->command_type = CmdType::kSelect;
    cte_query->can_set_tag = true;
    cte_query->target_list.push_back(MakeTargetEntry(MakeInt4Const(1), 1, "a"));
    cte_query->target_list.push_back(MakeTargetEntry(MakeInt4Const(2), 2, "b"));

    // Outer query: SELECT a, b FROM t.
    auto* outer = makePallocNode<Query>();
    outer->command_type = CmdType::kSelect;
    outer->can_set_tag = true;
    outer->rtable.push_back(MakeSubqueryRte(cte_query));
    outer->jointree = MakeFromExpr(MakeRangeTblRef(1));
    outer->target_list.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));
    outer->target_list.push_back(MakeTargetEntry(MakeVar(1, 2, kInt4Oid), 2, "b"));

    std::vector<TupleTableSlot*> slots;
    RunSelectAndGetSlots(outer, &slots);
    ASSERT_EQ(slots.size(), 1u);
    EXPECT_EQ(DatumGetInt32(slots[0]->tts_values[0]), 1);
    EXPECT_EQ(DatumGetInt32(slots[0]->tts_values[1]), 2);
}

// ===========================================================================
// SubTask 12.5.3 — CTE referenced multiple times in outer query
//
//   WITH t AS (SELECT 1 AS x) SELECT x, x+1, x+2 FROM t;
//
// The CTE's single column is referenced three times in the outer target
// list (directly and in two expressions). This exercises the
// SubqueryScan's projection of child columns into multiple outer outputs.
// ===========================================================================

TEST_F(CteExecutorTest, CteReferencedMultipleTimes) {
    // CTE body: SELECT 1 AS x.
    auto* cte_query = makePallocNode<Query>();
    cte_query->command_type = CmdType::kSelect;
    cte_query->can_set_tag = true;
    cte_query->target_list.push_back(MakeTargetEntry(MakeInt4Const(1), 1, "x"));

    // Outer query: SELECT x, x+1, x+2 FROM t.
    auto* outer = makePallocNode<Query>();
    outer->command_type = CmdType::kSelect;
    outer->can_set_tag = true;
    outer->rtable.push_back(MakeSubqueryRte(cte_query));
    outer->jointree = MakeFromExpr(MakeRangeTblRef(1));
    // Target list: x, x+1, x+2.
    outer->target_list.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "x"));
    outer->target_list.push_back(MakeTargetEntry(
        MakeOpExpr(kInt4PlusOp, kInt4Oid, MakeVar(1, 1, kInt4Oid), MakeInt4Const(1)), 2,
        "x_plus_1"));
    outer->target_list.push_back(MakeTargetEntry(
        MakeOpExpr(kInt4PlusOp, kInt4Oid, MakeVar(1, 1, kInt4Oid), MakeInt4Const(2)), 3,
        "x_plus_2"));

    std::vector<TupleTableSlot*> slots;
    RunSelectAndGetSlots(outer, &slots);
    ASSERT_EQ(slots.size(), 1u);
    EXPECT_EQ(DatumGetInt32(slots[0]->tts_values[0]), 1);  // x
    EXPECT_EQ(DatumGetInt32(slots[0]->tts_values[1]), 2);  // x+1
    EXPECT_EQ(DatumGetInt32(slots[0]->tts_values[2]), 3);  // x+2
}

// ===========================================================================
// SubTask 12.5.4 — CTE with WHERE clause (filtering in the outer query)
//
//   WITH t AS (SELECT 5 AS x) SELECT * FROM t WHERE x > 3;
//   WITH t AS (SELECT 5 AS x) SELECT * FROM t WHERE x > 10;
//
// Tests that a WHERE clause on the outer query is correctly attached to
// the SubqueryScan plan and evaluated against the CTE's output. The
// first variant passes the filter (returns 1 row); the second rejects
// the row (returns 0 rows).
//
// Note: A WHERE clause *inside* the CTE body that scans a real relation
// (e.g. WITH t AS (SELECT * FROM tbl WHERE cond) SELECT * FROM t) is not
// covered here. That scenario requires the planner's set_plan_references
// pass to flatten the CTE body's range table into the parent's range
// table, so the SeqScan inside the recursive plan can resolve its
// RangeTblEntry. That work is beyond Task 12's scope.
// ===========================================================================

TEST_F(CteExecutorTest, CteWithWhereClause) {
    // CTE body: SELECT 5 AS x.
    auto* cte_query = makePallocNode<Query>();
    cte_query->command_type = CmdType::kSelect;
    cte_query->can_set_tag = true;
    cte_query->target_list.push_back(MakeTargetEntry(MakeInt4Const(5), 1, "x"));

    // Outer query: SELECT x FROM t WHERE x > 3  (passes: 5 > 3).
    auto* outer = makePallocNode<Query>();
    outer->command_type = CmdType::kSelect;
    outer->can_set_tag = true;
    outer->rtable.push_back(MakeSubqueryRte(cte_query));
    Node* qual = MakeOpExpr(kInt4GtOp, kBoolOid, MakeVar(1, 1, kInt4Oid), MakeInt4Const(3));
    outer->jointree = MakeFromExpr(MakeRangeTblRef(1), qual);
    outer->target_list.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "x"));

    auto results = RunSelectSingleColumn(outer);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0], 5);
}

TEST_F(CteExecutorTest, CteWithWhereClauseRejectsRow) {
    // CTE body: SELECT 5 AS x.
    auto* cte_query = makePallocNode<Query>();
    cte_query->command_type = CmdType::kSelect;
    cte_query->can_set_tag = true;
    cte_query->target_list.push_back(MakeTargetEntry(MakeInt4Const(5), 1, "x"));

    // Outer query: SELECT x FROM t WHERE x > 10  (rejects: 5 <= 10).
    auto* outer = makePallocNode<Query>();
    outer->command_type = CmdType::kSelect;
    outer->can_set_tag = true;
    outer->rtable.push_back(MakeSubqueryRte(cte_query));
    Node* qual = MakeOpExpr(kInt4GtOp, kBoolOid, MakeVar(1, 1, kInt4Oid), MakeInt4Const(10));
    outer->jointree = MakeFromExpr(MakeRangeTblRef(1), qual);
    outer->target_list.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "x"));

    auto results = RunSelectSingleColumn(outer);
    EXPECT_EQ(results.size(), 0u);
}

}  // namespace

// optimizer_test.cpp — Unit tests for the optimizer/planner (M10).
//
// Tests the planner entry point (planner()), subplanner, path generation,
// and cost estimation. Verifies that the correct Plan tree is produced for:
//   - SELECT without FROM (Result plan)
//   - SELECT FROM table (SeqScan plan)
//   - SELECT with WHERE (SeqScan with qual)
//   - SELECT with GROUP BY (Agg plan)
//   - SELECT with ORDER BY (Sort plan)
//   - SELECT with LIMIT (Sort with Top-N limit)
//   - INSERT/UPDATE/DELETE (ModifyTable plan)
//   - Cost estimation (seqscan, index, sort, agg)

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <string>

#include "catalog/catalog.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "executor/plannodes.hpp"
#include "optimizer/cost.hpp"
#include "optimizer/path.hpp"
#include "optimizer/planner.hpp"
#include "parser/parsenodes.hpp"
#include "parser/primnodes.hpp"
#include "types/datum.hpp"

using pgcpp::catalog::Oid;
using pgcpp::executor::Agg;
using pgcpp::executor::ModifyTable;
using pgcpp::executor::Plan;
using pgcpp::executor::PlanType;
using pgcpp::executor::Result;
using pgcpp::executor::SeqScan;
using pgcpp::executor::Sort;
using pgcpp::memory::AllocSetContext;
using pgcpp::memory::palloc;
using pgcpp::optimizer::Cardinality;
using pgcpp::optimizer::ClampRowEst;
using pgcpp::optimizer::Cost;
using pgcpp::optimizer::CostAgg;
using pgcpp::optimizer::CostIndexScan;
using pgcpp::optimizer::CostSeqScan;
using pgcpp::optimizer::CostSort;
using pgcpp::optimizer::EstimateSelectivity;
using pgcpp::optimizer::IndexPath;
using pgcpp::optimizer::planner;
using pgcpp::optimizer::PlannerInfo;
using pgcpp::optimizer::RelOptInfo;
using pgcpp::optimizer::Selectivity;
using pgcpp::optimizer::SeqScanPath;
using pgcpp::parser::CmdType;
using pgcpp::parser::Const;
using pgcpp::parser::FromExpr;
using pgcpp::parser::JoinType;
using pgcpp::parser::Node;
using pgcpp::parser::OpExpr;
using pgcpp::parser::Query;
using pgcpp::parser::RangeTblEntry;
using pgcpp::parser::RangeTblRef;
using pgcpp::parser::RTEKind;
using pgcpp::parser::SortGroupClause;
using pgcpp::parser::TargetEntry;
using pgcpp::parser::Var;
using pgcpp::types::Int32GetDatum;
using pgcpp::types::Int64GetDatum;
using pgcpp::types::kInt4Oid;
using pgcpp::types::kInt8Oid;

namespace {

using pgcpp::nodes::makePallocNode;

// Operator OIDs (from bootstrap_catalog.cpp).
constexpr Oid kInt4EqOp = 96;   // int4 = int4
constexpr Oid kInt4LtOp = 97;   // int4 < int4
constexpr Oid kInt4GtOp = 521;  // int4 > int4

class OptimizerTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("optimizer_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);
    }

    void TearDown() override {
        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    // Helper: create a Var node.
    Var* MakeVar(int varno, int varattno, Oid vartype) {
        auto* var = makePallocNode<Var>();
        var->varno = varno;
        var->varattno = varattno;
        var->vartype = vartype;
        return var;
    }

    // Helper: create a Const node for int4.
    Const* MakeInt4Const(int32_t value) {
        auto* con = makePallocNode<Const>();
        con->consttype = kInt4Oid;
        con->constvalue = Int32GetDatum(value);
        con->constisnull = false;
        con->constbyval = true;
        con->constlen = 4;
        return con;
    }

    // Helper: create a Const node for int8 (used for LIMIT).
    Const* MakeInt8Const(int64_t value) {
        auto* con = makePallocNode<Const>();
        con->consttype = kInt8Oid;
        con->constvalue = Int64GetDatum(value);
        con->constisnull = false;
        con->constbyval = true;
        con->constlen = 8;
        return con;
    }

    // Helper: create a TargetEntry.
    TargetEntry* MakeTargetEntry(Node* expr, int resno, const std::string& resname = "",
                                 int ressortgroupref = 0) {
        auto* te = makePallocNode<TargetEntry>();
        te->expr = expr;
        te->resno = resno;
        te->resname = resname;
        te->ressortgroupref = ressortgroupref;
        return te;
    }

    // Helper: create an OpExpr.
    OpExpr* MakeOpExpr(Oid opno, Oid resulttype, Node* left, Node* right) {
        auto* op = makePallocNode<OpExpr>();
        op->opno = opno;
        op->opresulttype = resulttype;
        op->args.push_back(left);
        op->args.push_back(right);
        return op;
    }

    // Helper: create a SortGroupClause.
    SortGroupClause* MakeSortGroupClause(int tle_sort_group_ref, int sortop, bool nulls_first) {
        auto* sgc = makePallocNode<SortGroupClause>();
        sgc->tle_sort_group_ref = tle_sort_group_ref;
        sgc->sortop = sortop;
        sgc->nulls_first = nulls_first;
        return sgc;
    }

    // Helper: create a RangeTblEntry for a relation.
    RangeTblEntry* MakeRTE(int relid) {
        auto* rte = makePallocNode<RangeTblEntry>();
        rte->rtekind = RTEKind::kRelation;
        rte->relid = relid;
        return rte;
    }

    // Helper: create a RangeTblRef.
    RangeTblRef* MakeRangeTblRef(int rtindex) {
        auto* ref = makePallocNode<RangeTblRef>();
        ref->rtindex = rtindex;
        return ref;
    }

    // Helper: create a FromExpr with a single base relation.
    FromExpr* MakeFromExpr(int rtindex, Node* quals = nullptr) {
        auto* from = makePallocNode<FromExpr>();
        from->fromlist.push_back(MakeRangeTblRef(rtindex));
        from->quals = quals;
        return from;
    }

    // Helper: create a simple SELECT query.
    Query* MakeSelectQuery() {
        auto* query = makePallocNode<Query>();
        query->command_type = CmdType::kSelect;
        return query;
    }

    AllocSetContext* context_ = nullptr;
};

// === Plan generation tests ===

// SELECT 1 → Result plan with no children.
TEST_F(OptimizerTest, SelectNoFrom_ProducesResultPlan) {
    Query* query = MakeSelectQuery();
    query->target_list.push_back(MakeTargetEntry(MakeInt4Const(1), 1, "?column?"));

    Plan* plan = planner(query);

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PlanType::kResult);
    EXPECT_EQ(plan->lefttree, nullptr);
    EXPECT_EQ(plan->righttree, nullptr);
    EXPECT_EQ(plan->targetlist.size(), 1u);
}

// SELECT a FROM t → SeqScan plan.
TEST_F(OptimizerTest, SelectFromTable_ProducesSeqScanPlan) {
    Query* query = MakeSelectQuery();
    query->rtable.push_back(MakeRTE(16384));
    query->jointree = MakeFromExpr(1);
    query->target_list.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));

    Plan* plan = planner(query);

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PlanType::kSeqScan);
    auto* scan = static_cast<SeqScan*>(plan);
    EXPECT_EQ(scan->scanrelid, 1);
    EXPECT_EQ(scan->targetlist.size(), 1u);
    EXPECT_EQ(scan->qual, nullptr);
}

// SELECT a FROM t WHERE a > 5 → SeqScan with qual.
TEST_F(OptimizerTest, SelectWithWhere_ProducesSeqScanWithQual) {
    Node* qual =
        MakeOpExpr(kInt4GtOp, pgcpp::types::kBoolOid, MakeVar(1, 1, kInt4Oid), MakeInt4Const(5));

    Query* query = MakeSelectQuery();
    query->rtable.push_back(MakeRTE(16384));
    query->jointree = MakeFromExpr(1, qual);
    query->target_list.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a"));

    Plan* plan = planner(query);

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PlanType::kSeqScan);
    auto* scan = static_cast<SeqScan*>(plan);
    EXPECT_NE(scan->qual, nullptr);
}

// SELECT COUNT(*) FROM t → Agg plan on SeqScan.
TEST_F(OptimizerTest, SelectCount_ProducesAggOnSeqScan) {
    Query* query = MakeSelectQuery();
    query->rtable.push_back(MakeRTE(16384));
    query->jointree = MakeFromExpr(1);
    query->has_aggs = true;

    // COUNT(*) is an Aggref with aggstar=true.
    auto* agg = makePallocNode<pgcpp::parser::Aggref>();
    agg->aggfnoid = 2147;  // count(int4)
    agg->aggtype = kInt8Oid;
    agg->aggstar = true;

    query->target_list.push_back(MakeTargetEntry(agg, 1, "count"));

    Plan* plan = planner(query);

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PlanType::kAgg);
    auto* aggplan = static_cast<Agg*>(plan);
    EXPECT_EQ(aggplan->aggstrategy, Agg::Strategy::kPlain);
    ASSERT_NE(aggplan->lefttree, nullptr);
    EXPECT_EQ(aggplan->lefttree->type, PlanType::kSeqScan);
}

// SELECT a, COUNT(*) FROM t GROUP BY a → Agg (hashed) on SeqScan.
TEST_F(OptimizerTest, SelectGroupBy_ProducesHashedAggOnSeqScan) {
    Query* query = MakeSelectQuery();
    query->rtable.push_back(MakeRTE(16384));
    query->jointree = MakeFromExpr(1);
    query->has_aggs = true;

    // Target list: a (ressortgroupref=1), COUNT(*)
    query->target_list.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a", 1));

    auto* aggref = makePallocNode<pgcpp::parser::Aggref>();
    aggref->aggfnoid = 2147;
    aggref->aggtype = kInt8Oid;
    aggref->aggstar = true;
    query->target_list.push_back(MakeTargetEntry(aggref, 2, "count"));

    // GROUP BY a (references tle_sort_group_ref=1)
    query->group_clause.push_back(MakeSortGroupClause(1, kInt4EqOp, false));

    Plan* plan = planner(query);

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PlanType::kAgg);
    auto* aggplan = static_cast<Agg*>(plan);
    EXPECT_EQ(aggplan->aggstrategy, Agg::Strategy::kHashed);
    ASSERT_EQ(aggplan->groupColIdx.size(), 1u);
    EXPECT_EQ(aggplan->groupColIdx[0], 1);
    ASSERT_NE(aggplan->lefttree, nullptr);
    EXPECT_EQ(aggplan->lefttree->type, PlanType::kSeqScan);
}

// SELECT a FROM t ORDER BY a → Sort plan on SeqScan.
TEST_F(OptimizerTest, SelectOrderBy_ProducesSortOnSeqScan) {
    Query* query = MakeSelectQuery();
    query->rtable.push_back(MakeRTE(16384));
    query->jointree = MakeFromExpr(1);

    // Target list: a (ressortgroupref=1)
    query->target_list.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a", 1));

    // ORDER BY a ASC (sortop = 0, the ASC sentinel)
    query->sort_clause.push_back(MakeSortGroupClause(1, 0, false));

    Plan* plan = planner(query);

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PlanType::kSort);
    auto* sort = static_cast<Sort*>(plan);
    ASSERT_EQ(sort->sortColIdx.size(), 1u);
    EXPECT_EQ(sort->sortColIdx[0], 1);
    ASSERT_EQ(sort->sortOperators.size(), 1u);
    EXPECT_EQ(sort->sortOperators[0], 0);
    EXPECT_FALSE(sort->reverse[0]);  // ASC
    EXPECT_EQ(sort->limit, -1);      // No LIMIT
    ASSERT_NE(sort->lefttree, nullptr);
    EXPECT_EQ(sort->lefttree->type, PlanType::kSeqScan);
}

// SELECT a FROM t ORDER BY a DESC → Sort with reverse=true.
TEST_F(OptimizerTest, SelectOrderByDesc_ProducesSortWithReverse) {
    Query* query = MakeSelectQuery();
    query->rtable.push_back(MakeRTE(16384));
    query->jointree = MakeFromExpr(1);

    query->target_list.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a", 1));

    // ORDER BY a DESC (sortop = 1, the DESC sentinel)
    query->sort_clause.push_back(MakeSortGroupClause(1, 1, false));

    Plan* plan = planner(query);

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PlanType::kSort);
    auto* sort = static_cast<Sort*>(plan);
    EXPECT_TRUE(sort->reverse[0]);  // DESC
}

// SELECT a FROM t ORDER BY a LIMIT 10 → Sort with Top-N limit.
TEST_F(OptimizerTest, SelectOrderByLimit_ProducesSortWithTopN) {
    Query* query = MakeSelectQuery();
    query->rtable.push_back(MakeRTE(16384));
    query->jointree = MakeFromExpr(1);

    query->target_list.push_back(MakeTargetEntry(MakeVar(1, 1, kInt4Oid), 1, "a", 1));

    query->sort_clause.push_back(MakeSortGroupClause(1, 0, false));
    query->limit_count = MakeInt8Const(10);

    Plan* plan = planner(query);

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PlanType::kSort);
    auto* sort = static_cast<Sort*>(plan);
    EXPECT_EQ(sort->limit, 10);
}

// INSERT INTO t VALUES (1, 2) → ModifyTable wrapping Result.
TEST_F(OptimizerTest, Insert_ProducesModifyTable) {
    Query* query = MakeSelectQuery();
    query->command_type = CmdType::kInsert;
    query->result_relation = 1;
    query->rtable.push_back(MakeRTE(16384));
    // INSERT ... VALUES has no jointree (source is a Result).

    // Target list: two constants.
    query->target_list.push_back(MakeTargetEntry(MakeInt4Const(1), 1, "a"));
    query->target_list.push_back(MakeTargetEntry(MakeInt4Const(2), 2, "b"));

    Plan* plan = planner(query);

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PlanType::kModifyTable);
    auto* mt = static_cast<ModifyTable*>(plan);
    EXPECT_EQ(mt->operation, CmdType::kInsert);
    EXPECT_EQ(mt->resultRelid, 1);
    ASSERT_NE(mt->lefttree, nullptr);
}

// UPDATE t SET a = 1 → ModifyTable wrapping SeqScan.
TEST_F(OptimizerTest, Update_ProducesModifyTable) {
    Query* query = MakeSelectQuery();
    query->command_type = CmdType::kUpdate;
    query->result_relation = 1;
    query->rtable.push_back(MakeRTE(16384));
    query->jointree = MakeFromExpr(1);

    query->target_list.push_back(MakeTargetEntry(MakeInt4Const(1), 1, "a"));

    Plan* plan = planner(query);

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PlanType::kModifyTable);
    auto* mt = static_cast<ModifyTable*>(plan);
    EXPECT_EQ(mt->operation, CmdType::kUpdate);
    EXPECT_EQ(mt->resultRelid, 1);
    ASSERT_NE(mt->lefttree, nullptr);
    EXPECT_EQ(mt->lefttree->type, PlanType::kSeqScan);
}

// DELETE FROM t → ModifyTable wrapping SeqScan.
TEST_F(OptimizerTest, Delete_ProducesModifyTable) {
    Query* query = MakeSelectQuery();
    query->command_type = CmdType::kDelete;
    query->result_relation = 1;
    query->rtable.push_back(MakeRTE(16384));
    query->jointree = MakeFromExpr(1);

    Plan* plan = planner(query);

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PlanType::kModifyTable);
    auto* mt = static_cast<ModifyTable*>(plan);
    EXPECT_EQ(mt->operation, CmdType::kDelete);
    EXPECT_EQ(mt->resultRelid, 1);
    ASSERT_NE(mt->lefttree, nullptr);
    EXPECT_EQ(mt->lefttree->type, PlanType::kSeqScan);
}

// === Cost estimation tests ===

// SeqScan cost: pages * seq_page_cost + tuples * cpu_tuple_cost.
TEST_F(OptimizerTest, CostSeqScan_EstimatesCorrectly) {
    SeqScanPath path;
    CostSeqScan(&path, 10, 1000);
    // Expected: 10 * 1.0 + 1000 * 0.01 = 10 + 10 = 20
    EXPECT_NEAR(path.total_cost, 20.0, 0.01);
    EXPECT_NEAR(path.startup_cost, 0.0, 0.01);
    EXPECT_NEAR(path.rows, 1000.0, 0.01);
}

// IndexScan cost: fetched * random_page_cost + fetched * cpu_index_tuple_cost.
TEST_F(OptimizerTest, CostIndexScan_EstimatesCorrectly) {
    IndexPath path;
    CostIndexScan(&path, 1000, 0.1);
    // fetched = max(1, 1000 * 0.1) = 100
    // Expected: 100 * 4.0 + 100 * 0.005 + 100 * 0.01 = 400 + 0.5 + 1 = 401.5
    EXPECT_NEAR(path.total_cost, 401.5, 0.1);
    EXPECT_NEAR(path.rows, 100.0, 0.01);
}

// Sort cost: n * log(n) * operator_cost.
TEST_F(OptimizerTest, CostSort_EstimatesCorrectly) {
    Cost cost = CostSort(100, 4, -1);
    // Expected: 100 * log2(100) * 0.0025 ≈ 100 * 6.64 * 0.0025 ≈ 1.66
    EXPECT_GT(cost, 1.0);
    EXPECT_LT(cost, 3.0);
}

// Sort cost with Top-N limit: only sorts top N rows.
TEST_F(OptimizerTest, CostSort_WithLimit_EstimatesCorrectly) {
    Cost cost_full = CostSort(1000, 4, -1);
    Cost cost_topn = CostSort(1000, 4, 10);
    EXPECT_LT(cost_topn, cost_full);
}

// Agg cost: input_rows * operator_cost + num_groups * cpu_tuple_cost.
TEST_F(OptimizerTest, CostAgg_EstimatesCorrectly) {
    Cost cost = CostAgg(1000, 10, 4);
    // Expected: 1000 * 0.0025 + 10 * 0.01 = 2.5 + 0.1 = 2.6
    EXPECT_NEAR(cost, 2.6, 0.1);
}

// ClampRowEst: rows < 1 → 1.
TEST_F(OptimizerTest, ClampRowEst_FloorsToOne) {
    EXPECT_NEAR(ClampRowEst(0.5), 1.0, 0.01);
    EXPECT_NEAR(ClampRowEst(0.0), 1.0, 0.01);
    EXPECT_NEAR(ClampRowEst(100.0), 100.0, 0.01);
}

// EstimateSelectivity: equality qual → 0.1.
TEST_F(OptimizerTest, EstimateSelectivity_EqualityQual) {
    Node* qual =
        MakeOpExpr(kInt4EqOp, pgcpp::types::kBoolOid, MakeVar(1, 1, kInt4Oid), MakeInt4Const(5));
    Selectivity sel = EstimateSelectivity(qual, 1000);
    EXPECT_NEAR(sel, 0.1, 0.01);
}

// EstimateSelectivity: range qual → 0.33.
TEST_F(OptimizerTest, EstimateSelectivity_RangeQual) {
    Node* qual =
        MakeOpExpr(kInt4GtOp, pgcpp::types::kBoolOid, MakeVar(1, 1, kInt4Oid), MakeInt4Const(5));
    Selectivity sel = EstimateSelectivity(qual, 1000);
    EXPECT_NEAR(sel, 0.33, 0.01);
}

// EstimateSelectivity: nullptr → 1.0 (no qual).
TEST_F(OptimizerTest, EstimateSelectivity_NullQual) {
    Selectivity sel = EstimateSelectivity(nullptr, 1000);
    EXPECT_NEAR(sel, 1.0, 0.01);
}

}  // namespace

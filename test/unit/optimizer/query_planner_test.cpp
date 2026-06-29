// query_planner_test.cpp — Unit tests for the PG-style planner pipeline (M10 15.3).
//
// Tests standard_planner, subquery_planner, grouping_planner, and query_planner.
// Verifies that the complete PG-style pipeline (init → path generation →
// create_plan → set_plan_references) produces correct Plan trees for:
//   - SELECT without FROM (Result plan)
//   - SELECT FROM table (SeqScan plan)
//   - SELECT with WHERE (SeqScan with qual)
//   - SELECT COUNT(*) FROM t (Agg on SeqScan)
//   - SELECT a FROM t ORDER BY a (Sort on SeqScan)
//   - SELECT a, COUNT(*) FROM t GROUP BY a (Hashed Agg on SeqScan)

#include <gtest/gtest.h>

#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "executor/plannodes.hpp"
#include "optimizer/planner.hpp"
#include "parser/parsenodes.hpp"
#include "parser/primnodes.hpp"
#include "types/datum.hpp"

using pgcpp::executor::Agg;
using pgcpp::executor::Plan;
using pgcpp::executor::PlanType;
using pgcpp::executor::Result;
using pgcpp::executor::SeqScan;
using pgcpp::executor::Sort;
using pgcpp::nodes::makePallocNode;
using pgcpp::optimizer::grouping_planner;
using pgcpp::optimizer::PlannerInfo;
using pgcpp::optimizer::query_planner;
using pgcpp::optimizer::standard_planner;
using pgcpp::optimizer::subquery_planner;
using pgcpp::parser::CmdType;
using pgcpp::parser::Const;
using pgcpp::parser::FromExpr;
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
using pgcpp::types::kBoolOid;
using pgcpp::types::kInt4Oid;
using pgcpp::types::kInt8Oid;

namespace {

constexpr pgcpp::catalog::Oid kInt4EqOp = 96;   // int4 = int4
constexpr pgcpp::catalog::Oid kInt4GtOp = 521;  // int4 > int4

class QueryPlannerTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = pgcpp::memory::AllocSetContext::Create("query_planner_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);
    }

    void TearDown() override {
        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    Var* MakeVar(int varno, int varattno) {
        auto* var = makePallocNode<Var>();
        var->varno = varno;
        var->varattno = varattno;
        var->vartype = kInt4Oid;
        return var;
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

    OpExpr* MakeOpExpr(pgcpp::catalog::Oid opno, Node* left, Node* right) {
        auto* op = makePallocNode<OpExpr>();
        op->opno = opno;
        op->opresulttype = kBoolOid;
        op->args.push_back(left);
        op->args.push_back(right);
        return op;
    }

    TargetEntry* MakeTargetEntry(Node* expr, int resno, const std::string& resname = "",
                                 int ressortgroupref = 0) {
        auto* te = makePallocNode<TargetEntry>();
        te->expr = expr;
        te->resno = resno;
        te->resname = resname;
        te->ressortgroupref = ressortgroupref;
        return te;
    }

    SortGroupClause* MakeSortGroupClause(int tle_sort_group_ref, int sortop, bool nulls_first) {
        auto* sgc = makePallocNode<SortGroupClause>();
        sgc->tle_sort_group_ref = tle_sort_group_ref;
        sgc->sortop = sortop;
        sgc->nulls_first = nulls_first;
        return sgc;
    }

    RangeTblEntry* MakeRTE(int relid) {
        auto* rte = makePallocNode<RangeTblEntry>();
        rte->rtekind = RTEKind::kRelation;
        rte->relid = relid;
        return rte;
    }

    RangeTblRef* MakeRangeTblRef(int rtindex) {
        auto* ref = makePallocNode<RangeTblRef>();
        ref->rtindex = rtindex;
        return ref;
    }

    FromExpr* MakeFromExpr(int rtindex, Node* quals = nullptr) {
        auto* from = makePallocNode<FromExpr>();
        from->fromlist.push_back(MakeRangeTblRef(rtindex));
        from->quals = quals;
        return from;
    }

    Query* MakeSelectQuery() {
        auto* query = makePallocNode<Query>();
        query->command_type = CmdType::kSelect;
        return query;
    }

    pgcpp::memory::AllocSetContext* context_ = nullptr;
};

// standard_planner(SELECT 1) → Result plan.
TEST_F(QueryPlannerTest, StandardPlanner_SelectNoFrom_ProducesResultPlan) {
    auto* query = MakeSelectQuery();
    query->target_list.push_back(MakeTargetEntry(MakeInt4Const(1), 1, "?column?"));

    Plan* plan = standard_planner(query);

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PlanType::kResult);
    EXPECT_EQ(plan->lefttree, nullptr);
    EXPECT_EQ(plan->righttree, nullptr);
}

// standard_planner(SELECT a FROM t) → SeqScan plan.
TEST_F(QueryPlannerTest, StandardPlanner_SelectFromTable_ProducesSeqScanPlan) {
    auto* query = MakeSelectQuery();
    query->rtable.push_back(MakeRTE(16384));
    query->jointree = MakeFromExpr(1);
    query->target_list.push_back(MakeTargetEntry(MakeVar(1, 1), 1, "a"));

    Plan* plan = standard_planner(query);

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PlanType::kSeqScan);
    auto* scan = static_cast<SeqScan*>(plan);
    EXPECT_EQ(scan->scanrelid, 1);
}

// standard_planner(SELECT a FROM t WHERE a > 5) → SeqScan with qual.
TEST_F(QueryPlannerTest, StandardPlanner_SelectWithWhere_ProducesSeqScanWithQual) {
    auto* query = MakeSelectQuery();
    query->rtable.push_back(MakeRTE(16384));
    Node* qual = MakeOpExpr(kInt4GtOp, MakeVar(1, 1), MakeInt4Const(5));
    query->jointree = MakeFromExpr(1, qual);
    query->target_list.push_back(MakeTargetEntry(MakeVar(1, 1), 1, "a"));

    Plan* plan = standard_planner(query);

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PlanType::kSeqScan);
    auto* scan = static_cast<SeqScan*>(plan);
    EXPECT_NE(scan->qual, nullptr);
}

// standard_planner(SELECT COUNT(*) FROM t) → Agg on SeqScan.
TEST_F(QueryPlannerTest, StandardPlanner_SelectCount_ProducesAggOnSeqScan) {
    auto* query = MakeSelectQuery();
    query->rtable.push_back(MakeRTE(16384));
    query->jointree = MakeFromExpr(1);
    query->has_aggs = true;

    auto* aggref = makePallocNode<pgcpp::parser::Aggref>();
    aggref->aggfnoid = 2147;
    aggref->aggtype = kInt8Oid;
    aggref->aggstar = true;
    query->target_list.push_back(MakeTargetEntry(aggref, 1, "count"));

    Plan* plan = standard_planner(query);

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PlanType::kAgg);
    auto* agg = static_cast<Agg*>(plan);
    EXPECT_EQ(agg->aggstrategy, Agg::Strategy::kPlain);
    ASSERT_NE(agg->lefttree, nullptr);
    EXPECT_EQ(agg->lefttree->type, PlanType::kSeqScan);
}

// standard_planner(SELECT a, COUNT(*) FROM t GROUP BY a) → Hashed Agg.
TEST_F(QueryPlannerTest, StandardPlanner_SelectGroupBy_ProducesHashedAgg) {
    auto* query = MakeSelectQuery();
    query->rtable.push_back(MakeRTE(16384));
    query->jointree = MakeFromExpr(1);
    query->has_aggs = true;

    query->target_list.push_back(MakeTargetEntry(MakeVar(1, 1), 1, "a", 1));

    auto* aggref = makePallocNode<pgcpp::parser::Aggref>();
    aggref->aggfnoid = 2147;
    aggref->aggtype = kInt8Oid;
    aggref->aggstar = true;
    query->target_list.push_back(MakeTargetEntry(aggref, 2, "count"));

    query->group_clause.push_back(MakeSortGroupClause(1, kInt4EqOp, false));

    Plan* plan = standard_planner(query);

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PlanType::kAgg);
    auto* agg = static_cast<Agg*>(plan);
    EXPECT_EQ(agg->aggstrategy, Agg::Strategy::kHashed);
}

// standard_planner(SELECT a FROM t ORDER BY a) → Sort on SeqScan.
TEST_F(QueryPlannerTest, StandardPlanner_SelectOrderBy_ProducesSortOnSeqScan) {
    auto* query = MakeSelectQuery();
    query->rtable.push_back(MakeRTE(16384));
    query->jointree = MakeFromExpr(1);
    query->target_list.push_back(MakeTargetEntry(MakeVar(1, 1), 1, "a", 1));
    query->sort_clause.push_back(MakeSortGroupClause(1, 0, false));

    Plan* plan = standard_planner(query);

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PlanType::kSort);
    auto* sort = static_cast<Sort*>(plan);
    ASSERT_NE(sort->lefttree, nullptr);
    EXPECT_EQ(sort->lefttree->type, PlanType::kSeqScan);
    ASSERT_EQ(sort->sortColIdx.size(), 1u);
    EXPECT_EQ(sort->sortColIdx[0], 1);
}

// standard_planner(nullptr) → nullptr.
TEST_F(QueryPlannerTest, StandardPlanner_NullQuery_ReturnsNullptr) {
    EXPECT_EQ(standard_planner(nullptr), nullptr);
}

// subquery_planner delegates to grouping_planner and produces a plan.
TEST_F(QueryPlannerTest, SubqueryPlanner_ProducesPlan) {
    auto* root = makePallocNode<PlannerInfo>();
    auto* query = MakeSelectQuery();
    query->rtable.push_back(MakeRTE(16384));
    query->jointree = MakeFromExpr(1);
    query->target_list.push_back(MakeTargetEntry(MakeVar(1, 1), 1, "a"));

    Plan* plan = subquery_planner(root, query, /*parent_root=*/nullptr,
                                  /*has_recursion=*/false, /*tuple_fraction=*/0.0);

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PlanType::kSeqScan);
    EXPECT_EQ(root->parse, query);
}

// grouping_planner delegates to query_planner and produces a plan.
TEST_F(QueryPlannerTest, GroupingPlanner_ProducesPlan) {
    auto* root = makePallocNode<PlannerInfo>();
    auto* query = MakeSelectQuery();
    query->rtable.push_back(MakeRTE(16384));
    query->jointree = MakeFromExpr(1);
    query->target_list.push_back(MakeTargetEntry(MakeVar(1, 1), 1, "a"));
    root->parse = query;

    Plan* plan = grouping_planner(root, /*tuple_fraction=*/0.0, /*can_sort=*/true);

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PlanType::kSeqScan);
}

// query_planner builds a complete plan for a simple SELECT.
TEST_F(QueryPlannerTest, QueryPlanner_SimpleSelect_ProducesSeqScan) {
    auto* root = makePallocNode<PlannerInfo>();
    auto* query = MakeSelectQuery();
    query->rtable.push_back(MakeRTE(16384));
    query->jointree = MakeFromExpr(1);
    query->target_list.push_back(MakeTargetEntry(MakeVar(1, 1), 1, "a"));

    Plan* plan = query_planner(root, query);

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PlanType::kSeqScan);
}

// query_planner handles a no-FROM query (Result plan).
TEST_F(QueryPlannerTest, QueryPlanner_NoFrom_ProducesResultPlan) {
    auto* root = makePallocNode<PlannerInfo>();
    auto* query = MakeSelectQuery();
    query->target_list.push_back(MakeTargetEntry(MakeInt4Const(1), 1, "?column?"));

    Plan* plan = query_planner(root, query);

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PlanType::kResult);
}

// query_planner with aggregate produces Agg on SeqScan.
TEST_F(QueryPlannerTest, QueryPlanner_Aggregate_ProducesAggOnSeqScan) {
    auto* root = makePallocNode<PlannerInfo>();
    auto* query = MakeSelectQuery();
    query->rtable.push_back(MakeRTE(16384));
    query->jointree = MakeFromExpr(1);
    query->has_aggs = true;

    auto* aggref = makePallocNode<pgcpp::parser::Aggref>();
    aggref->aggfnoid = 2147;
    aggref->aggtype = kInt8Oid;
    aggref->aggstar = true;
    query->target_list.push_back(MakeTargetEntry(aggref, 1, "count"));

    Plan* plan = query_planner(root, query);

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PlanType::kAgg);
    auto* agg = static_cast<Agg*>(plan);
    ASSERT_NE(agg->lefttree, nullptr);
    EXPECT_EQ(agg->lefttree->type, PlanType::kSeqScan);
}

}  // namespace

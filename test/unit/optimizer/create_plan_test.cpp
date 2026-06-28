// create_plan_test.cpp — Unit tests for Path→Plan translation (M10 15.3).
//
// Tests create_plan and the individual builders (create_seqscan_plan,
// create_agg_plan, create_sort_plan, create_result_plan). Verifies that
// each Path subclass is correctly translated to the corresponding Plan.

#include "pgcpp/optimizer/plan/create_plan.hpp"

#include <gtest/gtest.h>

#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/alloc_set.hpp"
#include "pgcpp/common/memory/memory_context.hpp"
#include "pgcpp/executor/plannodes.hpp"
#include "pgcpp/optimizer/path.hpp"
#include "pgcpp/optimizer/planner.hpp"
#include "pgcpp/optimizer/util/pathnode.hpp"
#include "pgcpp/parser/parsenodes.hpp"
#include "pgcpp/parser/primnodes.hpp"
#include "pgcpp/types/datum.hpp"

using pgcpp::executor::Agg;
using pgcpp::executor::Plan;
using pgcpp::executor::PlanType;
using pgcpp::executor::Result;
using pgcpp::executor::SeqScan;
using pgcpp::executor::Sort;
using pgcpp::nodes::makePallocNode;
using pgcpp::optimizer::add_path;
using pgcpp::optimizer::AggPath;
using pgcpp::optimizer::cheapest_path;
using pgcpp::optimizer::create_agg_path;
using pgcpp::optimizer::create_plan;
using pgcpp::optimizer::create_result_path;
using pgcpp::optimizer::create_seqscan_path;
using pgcpp::optimizer::create_sort_path;
using pgcpp::optimizer::Path;
using pgcpp::optimizer::PathType;
using pgcpp::optimizer::PlannerInfo;
using pgcpp::optimizer::RelOptInfo;
using pgcpp::optimizer::ResultPath;
using pgcpp::optimizer::SeqScanPath;
using pgcpp::optimizer::SortPath;
using pgcpp::parser::CmdType;
using pgcpp::parser::Const;
using pgcpp::parser::FromExpr;
using pgcpp::parser::Node;
using pgcpp::parser::Query;
using pgcpp::parser::RangeTblEntry;
using pgcpp::parser::RangeTblRef;
using pgcpp::parser::RTEKind;
using pgcpp::parser::SortGroupClause;
using pgcpp::parser::TargetEntry;
using pgcpp::parser::Var;
using pgcpp::types::Int32GetDatum;
using pgcpp::types::kInt4Oid;
using pgcpp::types::kInt8Oid;

namespace {

class CreatePlanTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = pgcpp::memory::AllocSetContext::Create("create_plan_test_context");
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

    FromExpr* MakeFromExpr(int rtindex) {
        auto* from = makePallocNode<FromExpr>();
        from->fromlist.push_back(MakeRangeTblRef(rtindex));
        return from;
    }

    Query* MakeSelectQuery() {
        auto* query = makePallocNode<Query>();
        query->command_type = CmdType::kSelect;
        return query;
    }

    // Create a PlannerInfo with a single base relation and a SeqScan path.
    PlannerInfo* MakeRootWithSeqScan(Query* query) {
        auto* root = makePallocNode<PlannerInfo>();
        root->parse = query;

        // Set up simple_rte_array and simple_rel_array.
        root->simple_rte_array.push_back(MakeRTE(16384));
        auto* rel = makePallocNode<RelOptInfo>();
        rel->relid = 16384;
        rel->relindex = 1;
        rel->pages = 10;
        rel->tuples = 1000;
        rel->rows = 1000;
        rel->width = 24;
        root->simple_rel_array.push_back(rel);

        // Create and add a SeqScan path.
        SeqScanPath* path = create_seqscan_path(root, rel);
        add_path(rel, path);

        return root;
    }

    pgcpp::memory::AllocSetContext* context_ = nullptr;
};

// create_plan(SeqScanPath) → SeqScan plan.
TEST_F(CreatePlanTest, SeqScanPath_ProducesSeqScanPlan) {
    auto* query = MakeSelectQuery();
    query->rtable.push_back(MakeRTE(16384));
    query->jointree = MakeFromExpr(1);
    query->target_list.push_back(MakeTargetEntry(MakeVar(1, 1), 1, "a"));

    PlannerInfo* root = MakeRootWithSeqScan(query);
    RelOptInfo* rel = root->simple_rel_array[0];
    Path* best_path = cheapest_path(rel);

    Plan* plan = create_plan(root, best_path);

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PlanType::kSeqScan);
    auto* scan = static_cast<SeqScan*>(plan);
    EXPECT_EQ(scan->scanrelid, 1);
}

// create_plan(ResultPath) → Result plan.
TEST_F(CreatePlanTest, ResultPath_ProducesResultPlan) {
    auto* query = MakeSelectQuery();
    query->target_list.push_back(MakeTargetEntry(MakeInt4Const(1), 1, "?column?"));

    auto* root = makePallocNode<PlannerInfo>();
    root->parse = query;

    RelOptInfo dummy_rel;
    std::vector<Node*> quals;
    ResultPath* path = create_result_path(root, &dummy_rel, quals);

    Plan* plan = create_plan(root, path);

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PlanType::kResult);
    auto* result = static_cast<Result*>(plan);
    EXPECT_EQ(result->plan_rows, 1);
}

// create_plan(AggPath) → Agg plan on SeqScan.
TEST_F(CreatePlanTest, AggPath_ProducesAggOnSeqScan) {
    auto* query = MakeSelectQuery();
    query->rtable.push_back(MakeRTE(16384));
    query->jointree = MakeFromExpr(1);
    query->has_aggs = true;

    // COUNT(*) target.
    auto* aggref = makePallocNode<pgcpp::parser::Aggref>();
    aggref->aggfnoid = 2147;
    aggref->aggtype = kInt8Oid;
    aggref->aggstar = true;
    query->target_list.push_back(MakeTargetEntry(aggref, 1, "count"));

    PlannerInfo* root = MakeRootWithSeqScan(query);
    RelOptInfo* rel = root->simple_rel_array[0];
    Path* subpath = cheapest_path(rel);

    std::vector<Node*> group_clause;
    AggPath* agg_path = create_agg_path(root, rel, subpath, Agg::Strategy::kPlain, group_clause, 1);

    Plan* plan = create_plan(root, agg_path);

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PlanType::kAgg);
    auto* agg = static_cast<Agg*>(plan);
    EXPECT_EQ(agg->aggstrategy, Agg::Strategy::kPlain);
    ASSERT_NE(agg->lefttree, nullptr);
    EXPECT_EQ(agg->lefttree->type, PlanType::kSeqScan);
}

// create_plan(SortPath) → Sort plan on SeqScan.
TEST_F(CreatePlanTest, SortPath_ProducesSortOnSeqScan) {
    auto* query = MakeSelectQuery();
    query->rtable.push_back(MakeRTE(16384));
    query->jointree = MakeFromExpr(1);
    query->target_list.push_back(MakeTargetEntry(MakeVar(1, 1), 1, "a", 1));

    PlannerInfo* root = MakeRootWithSeqScan(query);
    RelOptInfo* rel = root->simple_rel_array[0];
    Path* subpath = cheapest_path(rel);

    std::vector<SortGroupClause*> pathkeys;
    pathkeys.push_back(MakeSortGroupClause(1, 0, false));
    SortPath* sort_path = create_sort_path(root, rel, subpath, pathkeys);

    Plan* plan = create_plan(root, sort_path);

    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PlanType::kSort);
    auto* sort = static_cast<Sort*>(plan);
    ASSERT_NE(sort->lefttree, nullptr);
    EXPECT_EQ(sort->lefttree->type, PlanType::kSeqScan);
}

// create_plan returns nullptr for a nullptr path.
TEST_F(CreatePlanTest, NullPath_ReturnsNullptr) {
    auto* root = makePallocNode<PlannerInfo>();
    EXPECT_EQ(create_plan(root, nullptr), nullptr);
}

// create_seqscan_plan sets scanrelid from the parent rel.
TEST_F(CreatePlanTest, CreateSeqScanPlan_SetsScanRelId) {
    auto* root = makePallocNode<PlannerInfo>();
    auto* path = makePallocNode<SeqScanPath>();
    auto* rel = makePallocNode<RelOptInfo>();
    rel->relindex = 2;
    path->parent_rel = rel;
    path->rows = 100;

    std::vector<TargetEntry*> tlist;
    std::vector<Node*> clauses;
    SeqScan* scan = create_seqscan_plan(root, path, tlist, clauses);

    ASSERT_NE(scan, nullptr);
    EXPECT_EQ(scan->scanrelid, 2);
    EXPECT_EQ(scan->qual, nullptr);
}

// create_agg_plan with kHashed sets groupColIdx.
TEST_F(CreatePlanTest, CreateAggPlan_HashedSetsGroupColIdx) {
    auto* query = MakeSelectQuery();
    query->rtable.push_back(MakeRTE(16384));
    query->jointree = MakeFromExpr(1);
    query->has_aggs = true;

    // Target list: a (ressortgroupref=1), COUNT(*)
    query->target_list.push_back(MakeTargetEntry(MakeVar(1, 1), 1, "a", 1));
    auto* aggref = makePallocNode<pgcpp::parser::Aggref>();
    aggref->aggfnoid = 2147;
    aggref->aggtype = kInt8Oid;
    aggref->aggstar = true;
    query->target_list.push_back(MakeTargetEntry(aggref, 2, "count"));

    // GROUP BY a
    query->group_clause.push_back(MakeSortGroupClause(1, 96, false));

    PlannerInfo* root = MakeRootWithSeqScan(query);
    RelOptInfo* rel = root->simple_rel_array[0];
    Path* subpath = cheapest_path(rel);

    AggPath* agg_path =
        create_agg_path(root, rel, subpath, Agg::Strategy::kHashed, query->group_clause, 10);

    Plan* plan = create_plan(root, agg_path);

    ASSERT_NE(plan, nullptr);
    auto* agg = static_cast<Agg*>(plan);
    EXPECT_EQ(agg->aggstrategy, Agg::Strategy::kHashed);
    EXPECT_EQ(agg->groupColIdx.size(), 1u);
}

// create_sort_plan builds sortColIdx from pathkeys.
TEST_F(CreatePlanTest, CreateSortPlan_BuildsSortColIdx) {
    auto* query = MakeSelectQuery();
    query->rtable.push_back(MakeRTE(16384));
    query->jointree = MakeFromExpr(1);
    query->target_list.push_back(MakeTargetEntry(MakeVar(1, 1), 1, "a", 1));

    PlannerInfo* root = MakeRootWithSeqScan(query);
    RelOptInfo* rel = root->simple_rel_array[0];
    Path* subpath = cheapest_path(rel);

    std::vector<SortGroupClause*> pathkeys;
    pathkeys.push_back(MakeSortGroupClause(1, 0, false));  // ASC
    SortPath* sort_path = create_sort_path(root, rel, subpath, pathkeys);

    Plan* plan = create_plan(root, sort_path);

    ASSERT_NE(plan, nullptr);
    auto* sort = static_cast<Sort*>(plan);
    ASSERT_EQ(sort->sortColIdx.size(), 1u);
    EXPECT_EQ(sort->sortColIdx[0], 1);
}

}  // namespace

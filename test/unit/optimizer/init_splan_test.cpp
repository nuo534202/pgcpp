// init_splan_test.cpp — Unit tests for planner initialization (M10 15.3).
//
// Tests query_planner_init, build_base_rel_infos, deconstruct_jointree, and
// distribute_quals_to_rels. Verifies that PlannerInfo is correctly populated
// with simple_rte_array, simple_rel_array, and baserestrictinfo.

#include "optimizer/plan/init_splan.hpp"

#include <gtest/gtest.h>

#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "optimizer/path.hpp"
#include "optimizer/planner.hpp"
#include "optimizer/util/restrictinfo.hpp"
#include "parser/parsenodes.hpp"
#include "parser/primnodes.hpp"
#include "types/datum.hpp"

using pgcpp::nodes::makePallocNode;
using pgcpp::nodes::NodeTag;
using pgcpp::optimizer::build_base_rel_infos;
using pgcpp::optimizer::deconstruct_jointree;
using pgcpp::optimizer::distribute_quals_to_rels;
using pgcpp::optimizer::PlannerInfo;
using pgcpp::optimizer::query_planner_init;
using pgcpp::optimizer::RelOptInfo;
using pgcpp::optimizer::RestrictInfo;
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
using pgcpp::types::Int32GetDatum;
using pgcpp::types::kBoolOid;
using pgcpp::types::kInt4Oid;

namespace {

constexpr pgcpp::catalog::Oid kInt4GtOp = 521;  // int4 > int4

class InitSplanTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = pgcpp::memory::AllocSetContext::Create("init_splan_test_context");
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

// build_base_rel_infos populates simple_rte_array and simple_rel_array.
TEST_F(InitSplanTest, BuildBaseRelInfos_PopulatesArrays) {
    auto* root = makePallocNode<PlannerInfo>();
    auto* query = MakeSelectQuery();
    query->rtable.push_back(MakeRTE(16384));
    root->parse = query;

    build_base_rel_infos(root);

    ASSERT_EQ(root->simple_rte_array.size(), 1u);
    ASSERT_EQ(root->simple_rel_array.size(), 1u);
    EXPECT_NE(root->simple_rte_array[0], nullptr);
    EXPECT_NE(root->simple_rel_array[0], nullptr);
    EXPECT_EQ(root->simple_rel_array[0]->relid, 16384);
    EXPECT_EQ(root->simple_rel_array[0]->relindex, 1);
}

// build_base_rel_infos fills catalog statistics.
TEST_F(InitSplanTest, BuildBaseRelInfos_FillsCatalogStats) {
    auto* root = makePallocNode<PlannerInfo>();
    auto* query = MakeSelectQuery();
    query->rtable.push_back(MakeRTE(16384));
    root->parse = query;

    build_base_rel_infos(root);

    RelOptInfo* rel = root->simple_rel_array[0];
    EXPECT_GT(rel->pages, 0);
    EXPECT_GT(rel->tuples, 0);
    EXPECT_GT(rel->width, 0);
}

// build_base_rel_infos skips non-relation RTEs.
TEST_F(InitSplanTest, BuildBaseRelInfos_SkipsNonRelationRte) {
    auto* root = makePallocNode<PlannerInfo>();
    auto* query = MakeSelectQuery();
    auto* rte = makePallocNode<RangeTblEntry>();
    rte->rtekind = RTEKind::kSubquery;  // not a relation
    query->rtable.push_back(rte);
    root->parse = query;

    build_base_rel_infos(root);

    ASSERT_EQ(root->simple_rte_array.size(), 1u);
    // The RTE is recorded, but simple_rel_array slot is nullptr (not built).
    EXPECT_EQ(root->simple_rel_array[0], nullptr);
}

// deconstruct_jointree builds base rels from the join tree.
TEST_F(InitSplanTest, DeconstructJointree_BuildsBaseRels) {
    auto* root = makePallocNode<PlannerInfo>();
    auto* query = MakeSelectQuery();
    query->rtable.push_back(MakeRTE(16384));
    query->jointree = MakeFromExpr(1);
    root->parse = query;

    // Pre-populate simple_rte_array (normally done by build_base_rel_infos).
    root->simple_rte_array.push_back(MakeRTE(16384));
    root->simple_rel_array.push_back(nullptr);

    deconstruct_jointree(root);

    EXPECT_NE(root->simple_rel_array[0], nullptr);
    EXPECT_EQ(root->simple_rel_array[0]->relindex, 1);
}

// distribute_quals_to_rels attaches single-table quals to baserestrictinfo.
TEST_F(InitSplanTest, DistributeQuals_AttachesToBaseRestrictInfo) {
    auto* root = makePallocNode<PlannerInfo>();
    root->simple_rte_array.push_back(MakeRTE(16384));
    root->simple_rel_array.push_back(nullptr);
    // Build the base rel so distribute_quals can find it.
    auto* rel = makePallocNode<RelOptInfo>();
    rel->relindex = 1;
    rel->relid = 16384;
    root->simple_rel_array[0] = rel;

    Node* qual = MakeOpExpr(kInt4GtOp, MakeVar(1, 1), MakeInt4Const(5));

    distribute_quals_to_rels(root, qual);

    ASSERT_EQ(rel->baserestrictinfo.size(), 1u);
    RestrictInfo* ri = rel->baserestrictinfo[0];
    ASSERT_NE(ri, nullptr);
    EXPECT_EQ(ri->clause, qual);
    EXPECT_FALSE(ri->can_join);
}

// distribute_quals_to_rels handles AND'd quals by splitting them.
TEST_F(InitSplanTest, DistributeQuals_SplitsAndClauses) {
    auto* root = makePallocNode<PlannerInfo>();
    root->simple_rte_array.push_back(MakeRTE(16384));
    auto* rel = makePallocNode<RelOptInfo>();
    rel->relindex = 1;
    rel->relid = 16384;
    root->simple_rel_array.push_back(rel);

    // a > 5 AND a < 10
    Node* c1 = MakeOpExpr(kInt4GtOp, MakeVar(1, 1), MakeInt4Const(5));
    Node* c2 = MakeOpExpr(kInt4GtOp, MakeVar(1, 1), MakeInt4Const(10));
    auto* and_expr = makePallocNode<pgcpp::parser::BoolExpr>();
    and_expr->boolop = pgcpp::parser::BoolExprType::kAnd;
    and_expr->args.push_back(c1);
    and_expr->args.push_back(c2);

    distribute_quals_to_rels(root, and_expr);

    EXPECT_EQ(rel->baserestrictinfo.size(), 2u);
}

// distribute_quals_to_rels handles nullptr quals (no-op).
TEST_F(InitSplanTest, DistributeQuals_NullQuals_NoOp) {
    auto* root = makePallocNode<PlannerInfo>();
    auto* rel = makePallocNode<RelOptInfo>();
    rel->relindex = 1;
    root->simple_rel_array.push_back(rel);

    distribute_quals_to_rels(root, nullptr);

    EXPECT_EQ(rel->baserestrictinfo.size(), 0u);
}

// query_planner_init initializes the full planner state.
TEST_F(InitSplanTest, QueryPlannerInit_InitializesFullState) {
    auto* root = makePallocNode<PlannerInfo>();
    auto* query = MakeSelectQuery();
    query->rtable.push_back(MakeRTE(16384));
    Node* qual = MakeOpExpr(kInt4GtOp, MakeVar(1, 1), MakeInt4Const(5));
    query->jointree = MakeFromExpr(1, qual);

    query_planner_init(root, query);

    EXPECT_EQ(root->parse, query);
    ASSERT_EQ(root->simple_rte_array.size(), 1u);
    ASSERT_EQ(root->simple_rel_array.size(), 1u);
    ASSERT_NE(root->simple_rel_array[0], nullptr);
    EXPECT_EQ(root->simple_rel_array[0]->baserestrictinfo.size(), 1u);
}

// query_planner_init handles a query with no jointree.
TEST_F(InitSplanTest, QueryPlannerInit_NoJointree) {
    auto* root = makePallocNode<PlannerInfo>();
    auto* query = MakeSelectQuery();
    query->rtable.push_back(MakeRTE(16384));
    // No jointree set.

    query_planner_init(root, query);

    EXPECT_EQ(root->parse, query);
    EXPECT_EQ(root->simple_rte_array.size(), 1u);
}

}  // namespace

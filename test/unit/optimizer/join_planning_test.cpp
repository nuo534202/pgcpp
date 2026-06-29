// join_planning_test.cpp — Unit tests for Task 15.15 (M10 join + subquery).
//
// Tests equivalence class derivation, canonical pathkeys, join path factories
// (NestLoop/HashJoin/MergeJoin), SubqueryScan path, and IN-sublink unfolding.
// Verifies the join planner can generate candidate paths for two-table queries
// and that SubLink nodes in WHERE are converted to join clauses.

#include <gtest/gtest.h>

#include "catalog/catalog.hpp"
#include "catalog/pg_operator.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "executor/plannodes.hpp"
#include "optimizer/path.hpp"
#include "optimizer/path/equivclass.hpp"
#include "optimizer/path/joinpath.hpp"
#include "optimizer/path/joinrels.hpp"
#include "optimizer/path/pathkeys.hpp"
#include "optimizer/plan/create_plan.hpp"
#include "optimizer/plan/subselect.hpp"
#include "optimizer/planner.hpp"
#include "optimizer/util/pathnode.hpp"
#include "optimizer/util/relnode.hpp"
#include "optimizer/util/restrictinfo.hpp"
#include "parser/parsenodes.hpp"
#include "parser/primnodes.hpp"
#include "types/datum.hpp"

using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_operator;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::Oid;
using pgcpp::catalog::OperatorKind;
using pgcpp::catalog::SetCatalog;
using pgcpp::executor::MergeJoin;
using pgcpp::executor::NestLoop;
using pgcpp::executor::Plan;
using pgcpp::executor::PlanType;
using pgcpp::executor::SeqScan;
using pgcpp::executor::SubqueryScan;
using pgcpp::memory::AllocSetContext;
using pgcpp::nodes::makePallocNode;
using pgcpp::optimizer::add_eq_class;
using pgcpp::optimizer::add_path;
using pgcpp::optimizer::add_paths_to_joinrel;
using pgcpp::optimizer::build_joinrels_for_level;
using pgcpp::optimizer::classify_restrictinfo;
using pgcpp::optimizer::compare_pathkeys;
using pgcpp::optimizer::convert_any_sublink_to_join;
using pgcpp::optimizer::create_mergejoin_path;
using pgcpp::optimizer::create_mergejoin_plan;
using pgcpp::optimizer::create_nestloop_path;
using pgcpp::optimizer::create_seqscan_path;
using pgcpp::optimizer::create_subqueryscan_path;
using pgcpp::optimizer::create_subqueryscan_plan;
using pgcpp::optimizer::EquivalenceClass;
using pgcpp::optimizer::EquivalenceMember;
using pgcpp::optimizer::find_ec_member_for_var;
using pgcpp::optimizer::find_ecs_for_rel;
using pgcpp::optimizer::generate_join_implied_equalities;
using pgcpp::optimizer::HashJoinPath;
using pgcpp::optimizer::make_canonical_pathkey;
using pgcpp::optimizer::make_restrictinfo;
using pgcpp::optimizer::MergeJoinPath;
using pgcpp::optimizer::NestLoopPath;
using pgcpp::optimizer::Path;
using pgcpp::optimizer::PathKey;
using pgcpp::optimizer::pathkeys_equal;
using pgcpp::optimizer::pathkeys_is_subset;
using pgcpp::optimizer::PathType;
using pgcpp::optimizer::PlannerInfo;
using pgcpp::optimizer::process_equivalence;
using pgcpp::optimizer::pull_up_sublinks;
using pgcpp::optimizer::Relids;
using pgcpp::optimizer::RelOptInfo;
using pgcpp::optimizer::RestrictInfo;
using pgcpp::optimizer::SeqScanPath;
using pgcpp::optimizer::SpecialJoinInfo;
using pgcpp::optimizer::SubqueryScanPath;
using pgcpp::parser::Alias;
using pgcpp::parser::BoolExpr;
using pgcpp::parser::BoolExprType;
using pgcpp::parser::FromExpr;
using pgcpp::parser::JoinType;
using pgcpp::parser::Node;
using pgcpp::parser::OpExpr;
using pgcpp::parser::Query;
using pgcpp::parser::RangeTblEntry;
using pgcpp::parser::RangeTblRef;
using pgcpp::parser::RTEKind;
using pgcpp::parser::SortGroupClause;
using pgcpp::parser::SubLink;
using pgcpp::parser::SubLinkType;
using pgcpp::parser::TargetEntry;
using pgcpp::parser::Var;
using pgcpp::types::kBoolOid;
using pgcpp::types::kInt4Oid;

namespace {

// Operator OIDs (from bootstrap_catalog.cpp).
constexpr Oid kInt4EqOp = 96;  // int4 = int4

class JoinPlanningTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("join_planning_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        // Set up a minimal catalog with the int4eq operator (OID 96) so
        // classify_restrictinfo can set mergejoinable/hashjoinable flags.
        catalog_ = new Catalog();
        SetCatalog(catalog_);
        auto* op_row = makePallocNode<FormData_pg_operator>();
        op_row->oid = kInt4EqOp;
        op_row->oprname = "=";
        op_row->oprkind = OperatorKind::kBinary;
        op_row->oprcanmerge = true;
        op_row->oprcanhash = true;
        op_row->oprleft = kInt4Oid;
        op_row->oprright = kInt4Oid;
        op_row->oprresult = kBoolOid;
        catalog_->InsertOperator(op_row);
    }

    void TearDown() override {
        SetCatalog(nullptr);
        delete catalog_;
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

    OpExpr* MakeEqOp(Node* left, Node* right) {
        auto* op = makePallocNode<OpExpr>();
        op->opno = kInt4EqOp;
        op->opresulttype = kBoolOid;
        op->args.push_back(left);
        op->args.push_back(right);
        return op;
    }

    RelOptInfo* MakeBaseRel(int relindex, Oid relid = 0) {
        auto* rel = makePallocNode<RelOptInfo>();
        rel->relindex = relindex;
        rel->relid = relid;
        rel->rows = 100;
        rel->width = 24;
        rel->pages = 10;
        rel->tuples = 100;
        rel->relids = {relindex};
        return rel;
    }

    SeqScanPath* MakeSeqScanPath(RelOptInfo* rel) {
        auto* path = makePallocNode<SeqScanPath>();
        path->parent_rel = rel;
        path->relid = rel->relid;
        path->rows = rel->rows;
        path->width = rel->width;
        path->startup_cost = 0;
        path->total_cost = 10;
        return path;
    }

    PlannerInfo* MakePlanner() {
        auto* root = makePallocNode<PlannerInfo>();
        auto* query = makePallocNode<Query>();
        query->command_type = pgcpp::parser::CmdType::kSelect;
        query->jointree = makePallocNode<FromExpr>();
        root->parse = query;
        return root;
    }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
};

// ---------------------------------------------------------------------------
// EquivalenceClass tests
// ---------------------------------------------------------------------------

// classify_restrictinfo sets mergejoinable/hashjoinable from the catalog.
TEST_F(JoinPlanningTest, ClassifyRestrictInfo_SetsMergeHashFlags) {
    auto* root = MakePlanner();
    Node* clause = MakeEqOp(MakeVar(1, 1), MakeVar(2, 1));
    Relids relids = {1, 2};
    auto* ri = make_restrictinfo(root, clause, true, false, relids, {}, 0);

    EXPECT_TRUE(classify_restrictinfo(ri));
    EXPECT_TRUE(ri->mergejoinable);
    EXPECT_TRUE(ri->hashjoinable);
    EXPECT_EQ(ri->opno, kInt4EqOp);
}

// classify_restrictinfo returns false for non-OpExpr clauses.
TEST_F(JoinPlanningTest, ClassifyRestrictInfo_RejectsNonOpExpr) {
    auto* root = MakePlanner();
    auto* var = MakeVar(1, 1);
    Relids relids = {1};
    auto* ri = make_restrictinfo(root, var, false, false, relids, {}, 0);

    EXPECT_FALSE(classify_restrictinfo(ri));
    EXPECT_FALSE(ri->mergejoinable);
}

// process_equivalence creates a new EC from a mergejoinable equality clause.
TEST_F(JoinPlanningTest, ProcessEquivalence_CreatesNewEC) {
    auto* root = MakePlanner();
    // a.x = b.y (Var(1,1) = Var(2,1))
    Node* clause = MakeEqOp(MakeVar(1, 1), MakeVar(2, 1));
    Relids relids = {1, 2};
    auto* ri = make_restrictinfo(root, clause, true, false, relids, {}, 0);
    classify_restrictinfo(ri);

    EXPECT_TRUE(process_equivalence(root, ri));
    ASSERT_EQ(root->eq_classes.size(), 1u);
    EquivalenceClass* ec = root->eq_classes[0];
    EXPECT_EQ(ec->ec_members.size(), 2u);
    EXPECT_EQ(ec->ec_relids.size(), 2u);
    EXPECT_EQ(ri->left_ec, ec);
    EXPECT_EQ(ri->right_ec, ec);
}

// process_equivalence merges two ECs when a new equality links them.
// a.x = b.y creates EC1; b.y = c.z merges into EC1 with 3 members.
TEST_F(JoinPlanningTest, ProcessEquivalence_MergesExistingECs) {
    auto* root = MakePlanner();

    // a.x = b.y → EC1 with {a.x, b.y}
    Node* c1 = MakeEqOp(MakeVar(1, 1), MakeVar(2, 1));
    auto* ri1 = make_restrictinfo(root, c1, true, false, {1, 2}, {}, 0);
    classify_restrictinfo(ri1);
    process_equivalence(root, ri1);
    ASSERT_EQ(root->eq_classes.size(), 1u);

    // b.y = c.z → merges into EC1, now {a.x, b.y, c.z}
    Node* c2 = MakeEqOp(MakeVar(2, 1), MakeVar(3, 1));
    auto* ri2 = make_restrictinfo(root, c2, true, false, {2, 3}, {}, 0);
    classify_restrictinfo(ri2);
    process_equivalence(root, ri2);
    ASSERT_EQ(root->eq_classes.size(), 1u);
    EXPECT_EQ(root->eq_classes[0]->ec_members.size(), 3u);
    EXPECT_EQ(root->eq_classes[0]->ec_relids.size(), 3u);
}

// find_ec_member_for_var returns the member matching a Var.
TEST_F(JoinPlanningTest, FindECMemberForVar_ReturnsMatchingMember) {
    auto* root = MakePlanner();
    Var* v1 = MakeVar(1, 1);
    Node* clause = MakeEqOp(v1, MakeVar(2, 1));
    auto* ri = make_restrictinfo(root, clause, true, false, {1, 2}, {}, 0);
    classify_restrictinfo(ri);
    process_equivalence(root, ri);

    EquivalenceMember* em = find_ec_member_for_var(root, v1);
    ASSERT_NE(em, nullptr);
    EXPECT_EQ(em->expr, v1);
}

// find_ecs_for_rel returns ECs touching a given relation.
TEST_F(JoinPlanningTest, FindECsForRel_ReturnsMatchingECs) {
    auto* root = MakePlanner();
    Node* clause = MakeEqOp(MakeVar(1, 1), MakeVar(2, 1));
    auto* ri = make_restrictinfo(root, clause, true, false, {1, 2}, {}, 0);
    classify_restrictinfo(ri);
    process_equivalence(root, ri);

    auto ecs1 = find_ecs_for_rel(root, 1);
    ASSERT_EQ(ecs1.size(), 1u);
    auto ecs3 = find_ecs_for_rel(root, 3);
    EXPECT_TRUE(ecs3.empty());
}

// generate_join_implied_equalities synthesizes a join clause for a shared EC.
TEST_F(JoinPlanningTest, GenerateJoinImpliedEqualities_SynthesizesClause) {
    auto* root = MakePlanner();
    // a.x = b.y creates EC with {a.x, b.y}
    Node* clause = MakeEqOp(MakeVar(1, 1), MakeVar(2, 1));
    auto* ri = make_restrictinfo(root, clause, true, false, {1, 2}, {}, 0);
    classify_restrictinfo(ri);
    process_equivalence(root, ri);

    auto* outer_rel = MakeBaseRel(1);
    auto* inner_rel = MakeBaseRel(2);
    auto implied = generate_join_implied_equalities(root, outer_rel, inner_rel);
    ASSERT_EQ(implied.size(), 1u);
    EXPECT_TRUE(implied[0]->mergejoinable);
    EXPECT_TRUE(implied[0]->hashjoinable);
}

// ---------------------------------------------------------------------------
// PathKey tests
// ---------------------------------------------------------------------------

// make_canonical_pathkey deduplicates against the planner's canonical list.
TEST_F(JoinPlanningTest, MakeCanonicalPathkey_Deduplicates) {
    auto* root = MakePlanner();
    auto* ec = makePallocNode<EquivalenceClass>();

    PathKey* pk1 = make_canonical_pathkey(root, ec, kInt4EqOp, false, false);
    PathKey* pk2 = make_canonical_pathkey(root, ec, kInt4EqOp, false, false);
    EXPECT_EQ(pk1, pk2);  // same pointer (deduplicated)
    EXPECT_EQ(root->canonical_pathkeys.size(), 1u);
}

// pathkeys_equal compares all four fields.
TEST_F(JoinPlanningTest, PathKeysEqual_ComparesAllFields) {
    auto* root = MakePlanner();
    auto* ec1 = makePallocNode<EquivalenceClass>();
    auto* ec2 = makePallocNode<EquivalenceClass>();

    PathKey* pk1 = make_canonical_pathkey(root, ec1, kInt4EqOp, false, false);
    PathKey* pk2 = make_canonical_pathkey(root, ec1, kInt4EqOp, false, false);
    PathKey* pk3 = make_canonical_pathkey(root, ec2, kInt4EqOp, false, false);

    EXPECT_TRUE(pathkeys_equal(pk1, pk2));
    EXPECT_FALSE(pathkeys_equal(pk1, pk3));
}

// pathkeys_is_subset returns true for prefix.
TEST_F(JoinPlanningTest, PathKeysIsSubset_PrefixIsSubset) {
    auto* root = MakePlanner();
    auto* ec = makePallocNode<EquivalenceClass>();
    PathKey* pk1 = make_canonical_pathkey(root, ec, kInt4EqOp, false, false);

    std::vector<PathKey*> a = {pk1};
    std::vector<PathKey*> b = {pk1, pk1};
    EXPECT_TRUE(pathkeys_is_subset(a, b));
    EXPECT_FALSE(pathkeys_is_subset(b, a));
}

// compare_pathkeys returns 0 for equal, -1 for prefix, 2 for incomparable.
TEST_F(JoinPlanningTest, ComparePathKeys_ReturnsCorrectOrder) {
    auto* root = MakePlanner();
    auto* ec = makePallocNode<EquivalenceClass>();
    PathKey* pk = make_canonical_pathkey(root, ec, kInt4EqOp, false, false);

    std::vector<PathKey*> a = {pk};
    std::vector<PathKey*> b = {pk, pk};
    EXPECT_EQ(compare_pathkeys(a, a), 0);
    EXPECT_EQ(compare_pathkeys(a, b), -1);
    EXPECT_EQ(compare_pathkeys(b, a), 1);
}

// ---------------------------------------------------------------------------
// Join path factory tests
// ---------------------------------------------------------------------------

// create_mergejoin_path builds a MergeJoinPath with correct type and clauses.
TEST_F(JoinPlanningTest, CreateMergeJoinPath_SetsTypeAndClauses) {
    auto* root = MakePlanner();
    auto* outer_rel = MakeBaseRel(1);
    auto* inner_rel = MakeBaseRel(2);
    auto* outer_path = MakeSeqScanPath(outer_rel);
    auto* inner_path = MakeSeqScanPath(inner_rel);

    Node* clause = MakeEqOp(MakeVar(1, 1), MakeVar(2, 1));
    MergeJoinPath* path =
        create_mergejoin_path(root, nullptr, outer_path, inner_path, {clause}, JoinType::kInner);
    ASSERT_NE(path, nullptr);
    EXPECT_EQ(path->type, PathType::kMergeJoin);
    EXPECT_EQ(path->jointype, JoinType::kInner);
    ASSERT_EQ(path->mergeclauses.size(), 1u);
    EXPECT_EQ(path->mergeclauses[0], clause);
    EXPECT_GT(path->total_cost, 0.0);
}

// create_subqueryscan_path wraps a subpath and inherits its row count.
TEST_F(JoinPlanningTest, CreateSubqueryScanPath_WrapsSubpath) {
    auto* root = MakePlanner();
    auto* rel = MakeBaseRel(1);
    auto* subpath = MakeSeqScanPath(rel);

    SubqueryScanPath* path = create_subqueryscan_path(root, rel, subpath, 1, {});
    ASSERT_NE(path, nullptr);
    EXPECT_EQ(path->type, PathType::kSubqueryScan);
    EXPECT_EQ(path->subpath, subpath);
    EXPECT_EQ(path->scanrelid, 1);
    EXPECT_EQ(path->rows, subpath->rows);
    EXPECT_GE(path->total_cost, subpath->total_cost);
}

// create_nestloop_path builds a NestLoopPath with the restrictlist attached.
TEST_F(JoinPlanningTest, CreateNestLoopPath_AttachesRestrictList) {
    auto* root = MakePlanner();
    auto* outer_rel = MakeBaseRel(1);
    auto* inner_rel = MakeBaseRel(2);
    auto* outer_path = MakeSeqScanPath(outer_rel);
    auto* inner_path = MakeSeqScanPath(inner_rel);

    Node* clause = MakeEqOp(MakeVar(1, 1), MakeVar(2, 1));
    auto* ri = make_restrictinfo(root, clause, true, false, {1, 2}, {}, 0);
    NestLoopPath* path = create_nestloop_path(root, nullptr, outer_path, inner_path, {ri});
    ASSERT_NE(path, nullptr);
    EXPECT_EQ(path->type, PathType::kNestLoop);
    ASSERT_EQ(path->restrictlist.size(), 1u);
    EXPECT_EQ(path->restrictlist[0], ri);
}

// ---------------------------------------------------------------------------
// Join path generation (add_paths_to_joinrel) tests
// ---------------------------------------------------------------------------

// add_paths_to_joinrel generates NestLoop + HashJoin + MergeJoin when an
// equality join clause exists (operator supports both merge and hash).
TEST_F(JoinPlanningTest, AddPathsToJoinRel_GeneratesAllThreeJoinPaths) {
    auto* root = MakePlanner();
    auto* outer_rel = MakeBaseRel(1);
    auto* inner_rel = MakeBaseRel(2);
    outer_rel->cheapest_path = MakeSeqScanPath(outer_rel);
    inner_rel->cheapest_path = MakeSeqScanPath(inner_rel);

    // Build a join clause a.x = b.y with merge/hash eligibility.
    Node* clause = MakeEqOp(MakeVar(1, 1), MakeVar(2, 1));
    auto* ri = make_restrictinfo(root, clause, true, false, {1, 2}, {}, 0);
    classify_restrictinfo(ri);

    auto* joinrel = makePallocNode<RelOptInfo>();
    joinrel->relids = {1, 2};
    auto* sjinfo = makePallocNode<SpecialJoinInfo>();
    sjinfo->jointype = JoinType::kInner;

    add_paths_to_joinrel(root, joinrel, outer_rel, inner_rel, sjinfo, {ri});

    // Should have at least 3 paths: NestLoop, HashJoin, MergeJoin.
    EXPECT_GE(joinrel->pathlist.size(), 3u);
    EXPECT_NE(joinrel->cheapest_path, nullptr);
}

// add_paths_to_joinrel generates only NestLoop when no merge/hash clause.
TEST_F(JoinPlanningTest, AddPathsToJoinRel_NestLoopOnlyWithoutEqClause) {
    auto* root = MakePlanner();
    auto* outer_rel = MakeBaseRel(1);
    auto* inner_rel = MakeBaseRel(2);
    outer_rel->cheapest_path = MakeSeqScanPath(outer_rel);
    inner_rel->cheapest_path = MakeSeqScanPath(inner_rel);

    // No restrictlist → only NestLoop with no clauses.
    auto* joinrel = makePallocNode<RelOptInfo>();
    joinrel->relids = {1, 2};
    auto* sjinfo = makePallocNode<SpecialJoinInfo>();
    sjinfo->jointype = JoinType::kInner;

    add_paths_to_joinrel(root, joinrel, outer_rel, inner_rel, sjinfo, {});
    ASSERT_EQ(joinrel->pathlist.size(), 1u);
    EXPECT_EQ(joinrel->pathlist[0]->type, PathType::kNestLoop);
}

// ---------------------------------------------------------------------------
// Plan translation tests (Path → Plan)
// ---------------------------------------------------------------------------

// create_mergejoin_plan builds a MergeJoin plan from a MergeJoinPath.
TEST_F(JoinPlanningTest, CreateMergeJoinPlan_BuildsCorrectPlan) {
    auto* root = MakePlanner();
    auto* outer_rel = MakeBaseRel(1);
    auto* inner_rel = MakeBaseRel(2);
    auto* outer_path = MakeSeqScanPath(outer_rel);
    auto* inner_path = MakeSeqScanPath(inner_rel);

    Node* clause = MakeEqOp(MakeVar(1, 1), MakeVar(2, 1));
    MergeJoinPath* path =
        create_mergejoin_path(root, nullptr, outer_path, inner_path, {clause}, JoinType::kInner);

    MergeJoin* plan = create_mergejoin_plan(root, path);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PlanType::kMergeJoin);
    EXPECT_EQ(plan->jointype, JoinType::kInner);
    ASSERT_EQ(plan->mergeclauses.size(), 1u);
    EXPECT_EQ(plan->mergeclauses[0], clause);
}

// create_subqueryscan_plan builds a SubqueryScan plan from a SubqueryScanPath.
TEST_F(JoinPlanningTest, CreateSubqueryScanPlan_BuildsCorrectPlan) {
    auto* root = MakePlanner();
    auto* rel = MakeBaseRel(1);
    auto* subpath = MakeSeqScanPath(rel);

    SubqueryScanPath* path = create_subqueryscan_path(root, rel, subpath, 1, {});
    SubqueryScan* plan = create_subqueryscan_plan(root, path);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PlanType::kSubqueryScan);
    EXPECT_EQ(plan->scanrelid, 1);
}

// create_plan dispatches kMergeJoin through create_join_plan.
TEST_F(JoinPlanningTest, CreatePlan_DispatchesMergeJoin) {
    auto* root = MakePlanner();
    auto* outer_rel = MakeBaseRel(1);
    auto* inner_rel = MakeBaseRel(2);
    auto* outer_path = MakeSeqScanPath(outer_rel);
    auto* inner_path = MakeSeqScanPath(inner_rel);

    Node* clause = MakeEqOp(MakeVar(1, 1), MakeVar(2, 1));
    MergeJoinPath* path =
        create_mergejoin_path(root, nullptr, outer_path, inner_path, {clause}, JoinType::kInner);

    Plan* plan = pgcpp::optimizer::create_plan(root, path);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PlanType::kMergeJoin);
}

// create_plan dispatches kSubqueryScan through create_scan_plan.
TEST_F(JoinPlanningTest, CreatePlan_DispatchesSubqueryScan) {
    auto* root = MakePlanner();
    auto* rel = MakeBaseRel(1);
    auto* subpath = MakeSeqScanPath(rel);

    SubqueryScanPath* path = create_subqueryscan_path(root, rel, subpath, 1, {});
    Plan* plan = pgcpp::optimizer::create_plan(root, path);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->type, PlanType::kSubqueryScan);
}

// ---------------------------------------------------------------------------
// Subquery unfolding (pull_up_sublinks) tests
// ---------------------------------------------------------------------------

// pull_up_sublinks converts an IN-sublink in WHERE to a join clause and
// appends a subquery RTE + RangeTblRef to the jointree.
TEST_F(JoinPlanningTest, PullUpSubLinks_ConvertsINSubLinkToJoin) {
    auto* root = MakePlanner();
    Query* query = root->parse;

    // Build: SELECT ... FROM t1 WHERE a IN (SELECT b FROM t2)
    // Range table: [t1, t2]
    auto* rte1 = makePallocNode<RangeTblEntry>();
    rte1->rtekind = RTEKind::kRelation;
    rte1->relid = 1001;
    query->rtable.push_back(rte1);

    auto* rte2 = makePallocNode<RangeTblEntry>();
    rte2->rtekind = RTEKind::kRelation;
    rte2->relid = 1002;
    query->rtable.push_back(rte2);

    // Jointree: FROM t1
    auto* from = makePallocNode<FromExpr>();
    auto* ref1 = makePallocNode<RangeTblRef>();
    ref1->rtindex = 1;
    from->fromlist.push_back(ref1);
    query->jointree = from;

    // Subquery: SELECT b FROM t2
    auto* subquery = makePallocNode<Query>();
    subquery->command_type = pgcpp::parser::CmdType::kSelect;
    auto* sub_rte = makePallocNode<RangeTblEntry>();
    sub_rte->rtekind = RTEKind::kRelation;
    sub_rte->relid = 1002;
    subquery->rtable.push_back(sub_rte);
    auto* sub_from = makePallocNode<FromExpr>();
    auto* sub_ref = makePallocNode<RangeTblRef>();
    sub_ref->rtindex = 1;
    sub_from->fromlist.push_back(sub_ref);
    subquery->jointree = sub_from;
    // Subquery target list: [b]
    auto* sub_var = MakeVar(1, 1);
    auto* sub_te = makePallocNode<TargetEntry>();
    sub_te->expr = sub_var;
    sub_te->resno = 1;
    subquery->target_list.push_back(sub_te);

    // SubLink: a IN (subquery)
    auto* sublink = makePallocNode<SubLink>();
    sublink->sublinktype = SubLinkType::kAny;
    sublink->testexpr = MakeVar(1, 1);  // a (column 1 of t1)
    sublink->subselect = subquery;

    // WHERE: a IN (subquery)
    from->quals = sublink;

    int unfolded = pull_up_sublinks(root);
    EXPECT_EQ(unfolded, 1);
    // The subquery RTE should have been appended to the parent query's rtable.
    EXPECT_EQ(query->rtable.size(), 3u);
    auto* new_rte = static_cast<RangeTblEntry*>(query->rtable[2]);
    EXPECT_EQ(new_rte->rtekind, RTEKind::kSubquery);
    // The jointree's fromlist should now include a RangeTblRef for the new RTE.
    ASSERT_EQ(from->fromlist.size(), 2u);
    // The qual should no longer be a SubLink; it should be an OpExpr.
    ASSERT_NE(from->quals, nullptr);
    EXPECT_EQ(from->quals->GetTag(), pgcpp::nodes::NodeTag::kOpExpr);
}

// pull_up_sublinks returns 0 when there are no SubLinks in WHERE.
TEST_F(JoinPlanningTest, PullUpSubLinks_NoOpWhenNoSubLinks) {
    auto* root = MakePlanner();
    auto* from = static_cast<FromExpr*>(root->parse->jointree);
    Node* qual = MakeEqOp(MakeVar(1, 1), MakeVar(2, 1));
    from->quals = qual;

    EXPECT_EQ(pull_up_sublinks(root), 0);
    EXPECT_EQ(from->quals, qual);  // unchanged
}

}  // namespace

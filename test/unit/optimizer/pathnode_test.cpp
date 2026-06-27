// pathnode_test.cpp — Unit tests for Path factory functions (M10 15.3).
//
// Tests create_seqscan_path, create_index_path, create_nestloop_path,
// create_hashjoin_path, create_sort_path, create_agg_path, create_result_path,
// add_path, and cheapest_path. Verifies that each Path subclass is correctly
// initialized with its type, parent_rel, and cost estimates.

#include "mytoydb/optimizer/util/pathnode.hpp"

#include <gtest/gtest.h>

#include "mytoydb/common/containers/node.hpp"
#include "mytoydb/common/error/elog.hpp"
#include "mytoydb/common/memory/alloc_set.hpp"
#include "mytoydb/common/memory/memory_context.hpp"
#include "mytoydb/executor/plannodes.hpp"
#include "mytoydb/optimizer/path.hpp"
#include "mytoydb/optimizer/planner.hpp"
#include "mytoydb/parser/parsenodes.hpp"
#include "mytoydb/parser/primnodes.hpp"

using mytoydb::executor::Agg;
using mytoydb::nodes::makePallocNode;
using mytoydb::optimizer::add_path;
using mytoydb::optimizer::AggPath;
using mytoydb::optimizer::cheapest_path;
using mytoydb::optimizer::create_agg_path;
using mytoydb::optimizer::create_hashjoin_path;
using mytoydb::optimizer::create_index_path;
using mytoydb::optimizer::create_nestloop_path;
using mytoydb::optimizer::create_result_path;
using mytoydb::optimizer::create_seqscan_path;
using mytoydb::optimizer::create_sort_path;
using mytoydb::optimizer::HashJoinPath;
using mytoydb::optimizer::IndexPath;
using mytoydb::optimizer::NestLoopPath;
using mytoydb::optimizer::Path;
using mytoydb::optimizer::PathType;
using mytoydb::optimizer::PlannerInfo;
using mytoydb::optimizer::RelOptInfo;
using mytoydb::optimizer::ResultPath;
using mytoydb::optimizer::SeqScanPath;
using mytoydb::optimizer::SortPath;
using mytoydb::parser::SortGroupClause;

namespace {

class PathNodeTest : public ::testing::Test {
protected:
    void SetUp() override {
        mytoydb::error::InitErrorSubsystem();
        context_ = mytoydb::memory::AllocSetContext::Create("pathnode_test_context");
        mytoydb::memory::SetCurrentMemoryContext(context_);
    }

    void TearDown() override {
        mytoydb::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    // Create a RelOptInfo with sane defaults for testing.
    RelOptInfo* MakeRel(int relid = 16384, int relindex = 1) {
        auto* rel = makePallocNode<RelOptInfo>();
        rel->relid = relid;
        rel->relindex = relindex;
        rel->pages = 10;
        rel->tuples = 1000;
        rel->rows = 1000;
        rel->width = 24;
        return rel;
    }

    mytoydb::memory::AllocSetContext* context_ = nullptr;
};

// create_seqscan_path sets the type, parent_rel, and cost.
TEST_F(PathNodeTest, CreateSeqScanPath_SetsTypeAndCost) {
    auto* root = makePallocNode<PlannerInfo>();
    RelOptInfo* rel = MakeRel();

    SeqScanPath* path = create_seqscan_path(root, rel);

    ASSERT_NE(path, nullptr);
    EXPECT_EQ(path->type, PathType::kSeqScan);
    EXPECT_EQ(path->parent_rel, rel);
    EXPECT_EQ(path->relid, 16384);
    EXPECT_GT(path->total_cost, 0.0);
    EXPECT_EQ(path->rows, 1000.0);
}

// create_index_path stores the indexid and indexqual.
TEST_F(PathNodeTest, CreateIndexPath_SetsIndexId) {
    auto* root = makePallocNode<PlannerInfo>();
    RelOptInfo* rel = MakeRel();

    std::vector<mytoydb::parser::Node*> quals;
    IndexPath* path = create_index_path(root, rel, /*indexid=*/42, quals);

    ASSERT_NE(path, nullptr);
    EXPECT_EQ(path->type, PathType::kIndexScan);
    EXPECT_EQ(path->indexid, 42u);
    EXPECT_EQ(path->parent_rel, rel);
}

// create_nestloop_path sets outer/inner subpaths.
TEST_F(PathNodeTest, CreateNestLoopPath_SetsSubpaths) {
    auto* root = makePallocNode<PlannerInfo>();
    RelOptInfo* joinrel = MakeRel();
    RelOptInfo* outer_rel = MakeRel(16384, 1);
    RelOptInfo* inner_rel = MakeRel(16385, 2);

    Path* outer = create_seqscan_path(root, outer_rel);
    Path* inner = create_seqscan_path(root, inner_rel);

    std::vector<mytoydb::optimizer::RestrictInfo*> restrictlist;
    NestLoopPath* path = create_nestloop_path(root, joinrel, outer, inner, restrictlist);

    ASSERT_NE(path, nullptr);
    EXPECT_EQ(path->type, PathType::kNestLoop);
    EXPECT_EQ(path->outer, outer);
    EXPECT_EQ(path->inner, inner);
    EXPECT_EQ(path->parent_rel, joinrel);
}

// create_hashjoin_path sets the hashclauses.
TEST_F(PathNodeTest, CreateHashJoinPath_SetsHashClauses) {
    auto* root = makePallocNode<PlannerInfo>();
    RelOptInfo* joinrel = MakeRel();
    RelOptInfo* outer_rel = MakeRel(16384, 1);
    RelOptInfo* inner_rel = MakeRel(16385, 2);

    Path* outer = create_seqscan_path(root, outer_rel);
    Path* inner = create_seqscan_path(root, inner_rel);

    std::vector<mytoydb::parser::Node*> hashclauses;
    HashJoinPath* path = create_hashjoin_path(root, joinrel, outer, inner, hashclauses);

    ASSERT_NE(path, nullptr);
    EXPECT_EQ(path->type, PathType::kHashJoin);
    EXPECT_EQ(path->outer, outer);
    EXPECT_EQ(path->inner, inner);
}

// create_sort_path wraps a subpath with sort cost.
TEST_F(PathNodeTest, CreateSortPath_WrapsSubpath) {
    auto* root = makePallocNode<PlannerInfo>();
    RelOptInfo* rel = MakeRel();
    Path* subpath = create_seqscan_path(root, rel);

    std::vector<SortGroupClause*> pathkeys;
    SortPath* path = create_sort_path(root, rel, subpath, pathkeys);

    ASSERT_NE(path, nullptr);
    EXPECT_EQ(path->type, PathType::kSort);
    EXPECT_EQ(path->subpath, subpath);
    // Sort startup cost = subpath total cost (must read all before first output).
    EXPECT_GE(path->startup_cost, subpath->total_cost);
    EXPECT_GT(path->total_cost, path->startup_cost);
}

// create_agg_path sets the strategy and group_clause.
TEST_F(PathNodeTest, CreateAggPath_SetsStrategy) {
    auto* root = makePallocNode<PlannerInfo>();
    RelOptInfo* rel = MakeRel();
    Path* subpath = create_seqscan_path(root, rel);

    std::vector<mytoydb::parser::Node*> group_clause;
    AggPath* path = create_agg_path(root, rel, subpath, Agg::Strategy::kPlain, group_clause, 1);

    ASSERT_NE(path, nullptr);
    EXPECT_EQ(path->type, PathType::kAgg);
    EXPECT_EQ(path->subpath, subpath);
    EXPECT_EQ(path->aggstrategy, Agg::Strategy::kPlain);
    // Plain agg produces 1 row.
    EXPECT_EQ(path->rows, 1.0);
}

// create_agg_path with kHashed estimates group count.
TEST_F(PathNodeTest, CreateAggPath_HashedStrategy) {
    auto* root = makePallocNode<PlannerInfo>();
    RelOptInfo* rel = MakeRel();
    Path* subpath = create_seqscan_path(root, rel);

    std::vector<mytoydb::parser::Node*> group_clause;
    AggPath* path = create_agg_path(root, rel, subpath, Agg::Strategy::kHashed, group_clause, 10);

    ASSERT_NE(path, nullptr);
    EXPECT_EQ(path->aggstrategy, Agg::Strategy::kHashed);
    EXPECT_EQ(path->num_groups, 10);
    EXPECT_EQ(path->rows, 10.0);
}

// create_result_path for no-FROM queries.
TEST_F(PathNodeTest, CreateResultPath_NoFromQuery) {
    auto* root = makePallocNode<PlannerInfo>();
    RelOptInfo dummy_rel;

    std::vector<mytoydb::parser::Node*> quals;
    ResultPath* path = create_result_path(root, &dummy_rel, quals);

    ASSERT_NE(path, nullptr);
    EXPECT_EQ(path->type, PathType::kResult);
    EXPECT_EQ(path->rows, 1.0);
}

// add_path updates cheapest_path when the new path is cheaper.
TEST_F(PathNodeTest, AddPath_UpdatesCheapestPath) {
    auto* root = makePallocNode<PlannerInfo>();
    RelOptInfo* rel = MakeRel();

    EXPECT_EQ(rel->cheapest_path, nullptr);
    EXPECT_EQ(rel->pathlist.size(), 0u);

    SeqScanPath* p1 = create_seqscan_path(root, rel);
    add_path(rel, p1);

    EXPECT_EQ(rel->cheapest_path, p1);
    EXPECT_EQ(rel->pathlist.size(), 1u);
}

// add_path keeps the cheapest path when a more expensive one is added.
TEST_F(PathNodeTest, AddPath_KeepsCheapestWhenMoreExpensive) {
    auto* root = makePallocNode<PlannerInfo>();
    RelOptInfo* rel = MakeRel();

    SeqScanPath* p1 = create_seqscan_path(root, rel);
    add_path(rel, p1);

    // Create a second path with higher cost.
    auto* p2 = makePallocNode<SeqScanPath>();
    p2->parent_rel = rel;
    p2->total_cost = p1->total_cost + 1000.0;
    p2->rows = rel->rows;
    p2->width = rel->width;
    add_path(rel, p2);

    EXPECT_EQ(rel->cheapest_path, p1);
    EXPECT_EQ(rel->pathlist.size(), 2u);
}

// cheapest_path returns nullptr for an empty rel.
TEST_F(PathNodeTest, CheapestPath_EmptyRel) {
    auto* root = makePallocNode<PlannerInfo>();
    RelOptInfo* rel = MakeRel();

    EXPECT_EQ(cheapest_path(rel), nullptr);
}

}  // namespace

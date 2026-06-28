// relnode_test.cpp — Unit tests for RelOptInfo construction (M10 15.3).
//
// Tests build_simple_rel, build_join_rel, find_base_rel, find_join_rel, and
// add_join_rel. Verifies that RelOptInfo objects are correctly constructed
// and cached in the planner's simple_rel_array.

#include "pgcpp/optimizer/util/relnode.hpp"

#include <gtest/gtest.h>

#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/alloc_set.hpp"
#include "pgcpp/common/memory/memory_context.hpp"
#include "pgcpp/optimizer/path.hpp"
#include "pgcpp/optimizer/planner.hpp"
#include "pgcpp/parser/parsenodes.hpp"
#include "pgcpp/parser/primnodes.hpp"

using mytoydb::nodes::makePallocNode;
using mytoydb::optimizer::add_join_rel;
using mytoydb::optimizer::build_join_rel;
using mytoydb::optimizer::build_simple_rel;
using mytoydb::optimizer::find_base_rel;
using mytoydb::optimizer::find_join_rel;
using mytoydb::optimizer::PlannerInfo;
using mytoydb::optimizer::Relids;
using mytoydb::optimizer::RelOptInfo;
using mytoydb::optimizer::SpecialJoinInfo;
using mytoydb::parser::RangeTblEntry;
using mytoydb::parser::RTEKind;

namespace {

class RelNodeTest : public ::testing::Test {
protected:
    void SetUp() override {
        mytoydb::error::InitErrorSubsystem();
        context_ = mytoydb::memory::AllocSetContext::Create("relnode_test_context");
        mytoydb::memory::SetCurrentMemoryContext(context_);
    }

    void TearDown() override {
        mytoydb::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    // Create a PlannerInfo with a single base RTE in simple_rte_array.
    PlannerInfo* MakeRootWithOneRte(int relid = 16384) {
        auto* root = makePallocNode<PlannerInfo>();
        auto* rte = makePallocNode<RangeTblEntry>();
        rte->rtekind = RTEKind::kRelation;
        rte->relid = relid;
        root->simple_rte_array.push_back(rte);
        root->simple_rel_array.push_back(nullptr);
        return root;
    }

    // Create a PlannerInfo with two base RTEs.
    PlannerInfo* MakeRootWithTwoRtes(int relid1 = 16384, int relid2 = 16385) {
        auto* root = makePallocNode<PlannerInfo>();

        auto* rte1 = makePallocNode<RangeTblEntry>();
        rte1->rtekind = RTEKind::kRelation;
        rte1->relid = relid1;
        root->simple_rte_array.push_back(rte1);
        root->simple_rel_array.push_back(nullptr);

        auto* rte2 = makePallocNode<RangeTblEntry>();
        rte2->rtekind = RTEKind::kRelation;
        rte2->relid = relid2;
        root->simple_rte_array.push_back(rte2);
        root->simple_rel_array.push_back(nullptr);

        return root;
    }

    mytoydb::memory::AllocSetContext* context_ = nullptr;
};

// build_simple_rel creates a RelOptInfo with correct relid and relindex.
TEST_F(RelNodeTest, BuildSimpleRel_CreatesRelOptInfo) {
    PlannerInfo* root = MakeRootWithOneRte(16384);

    RelOptInfo* rel = build_simple_rel(root, /*relid=*/1, nullptr);

    ASSERT_NE(rel, nullptr);
    EXPECT_EQ(rel->relid, 16384);
    EXPECT_EQ(rel->relindex, 1);
    EXPECT_NE(rel->rte, nullptr);
}

// build_simple_rel caches the RelOptInfo in simple_rel_array.
TEST_F(RelNodeTest, BuildSimpleRel_CachesInArray) {
    PlannerInfo* root = MakeRootWithOneRte(16384);

    RelOptInfo* rel1 = build_simple_rel(root, 1, nullptr);
    RelOptInfo* rel2 = build_simple_rel(root, 1, nullptr);

    // Should return the same object (cached).
    EXPECT_EQ(rel1, rel2);
    EXPECT_EQ(root->simple_rel_array[0], rel1);
}

// build_simple_rel returns nullptr for an invalid relid.
TEST_F(RelNodeTest, BuildSimpleRel_InvalidRelid) {
    PlannerInfo* root = MakeRootWithOneRte(16384);

    EXPECT_EQ(build_simple_rel(root, 0, nullptr), nullptr);   // 0 is invalid
    EXPECT_EQ(build_simple_rel(root, 2, nullptr), nullptr);   // out of range
    EXPECT_EQ(build_simple_rel(root, -1, nullptr), nullptr);  // negative
}

// find_base_rel returns the RelOptInfo by index.
TEST_F(RelNodeTest, FindBaseRel_ReturnsRel) {
    PlannerInfo* root = MakeRootWithOneRte(16384);
    RelOptInfo* built = build_simple_rel(root, 1, nullptr);

    RelOptInfo* found = find_base_rel(root, 1);

    EXPECT_EQ(found, built);
}

// find_base_rel returns nullptr for an invalid index.
TEST_F(RelNodeTest, FindBaseRel_InvalidIndex) {
    PlannerInfo* root = MakeRootWithOneRte(16384);

    EXPECT_EQ(find_base_rel(root, 0), nullptr);
    EXPECT_EQ(find_base_rel(root, 2), nullptr);
}

// find_base_rel returns nullptr for an unbuilt slot.
TEST_F(RelNodeTest, FindBaseRel_UnbuiltSlot) {
    PlannerInfo* root = MakeRootWithOneRte(16384);
    // Don't call build_simple_rel first.

    EXPECT_EQ(find_base_rel(root, 1), nullptr);
}

// build_join_rel creates a join RelOptInfo and adds it to join_rel_list.
TEST_F(RelNodeTest, BuildJoinRel_AddsToJoinRelList) {
    PlannerInfo* root = MakeRootWithTwoRtes();
    RelOptInfo* outer = build_simple_rel(root, 1, nullptr);
    RelOptInfo* inner = build_simple_rel(root, 2, nullptr);

    Relids joinrelids = {1, 2};
    auto* sjinfo = makePallocNode<SpecialJoinInfo>();
    std::vector<mytoydb::optimizer::RestrictInfo*> restrictlist;

    RelOptInfo* joinrel = build_join_rel(root, joinrelids, outer, inner, sjinfo, restrictlist);

    ASSERT_NE(joinrel, nullptr);
    EXPECT_EQ(root->join_rel_list.size(), 1u);
    EXPECT_EQ(root->join_rel_list[0], joinrel);
    // Width should be the sum of outer and inner widths.
    EXPECT_GE(joinrel->width, outer->width);
}

// add_join_rel appends to join_rel_list.
TEST_F(RelNodeTest, AddJoinRel_AppendsToList) {
    PlannerInfo* root = MakeRootWithTwoRtes();

    auto* rel1 = makePallocNode<RelOptInfo>();
    auto* rel2 = makePallocNode<RelOptInfo>();

    add_join_rel(root, rel1);
    EXPECT_EQ(root->join_rel_list.size(), 1u);

    add_join_rel(root, rel2);
    EXPECT_EQ(root->join_rel_list.size(), 2u);
    EXPECT_EQ(root->join_rel_list[0], rel1);
    EXPECT_EQ(root->join_rel_list[1], rel2);
}

// find_join_rel is a skeleton that always returns nullptr.
TEST_F(RelNodeTest, FindJoinRel_ReturnsNullptr) {
    PlannerInfo* root = MakeRootWithTwoRtes();
    Relids joinrelids = {1, 2};

    EXPECT_EQ(find_join_rel(root, joinrelids), nullptr);
}

}  // namespace

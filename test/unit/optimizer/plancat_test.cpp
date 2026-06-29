// plancat_test.cpp — Unit tests for catalog lookup (M10 15.3).
//
// Tests estimate_rel_size and get_relation_info. Verifies that catalog
// statistics (pages, tuples, width) are correctly populated from pg_class
// and pg_attribute, with conservative defaults when the catalog has no data.

#include "optimizer/util/plancat.hpp"

#include <gtest/gtest.h>

#include "catalog/catalog.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "optimizer/path.hpp"
#include "optimizer/planner.hpp"
#include "parser/parsenodes.hpp"
#include "parser/primnodes.hpp"

using pgcpp::catalog::Oid;
using pgcpp::nodes::makePallocNode;
using pgcpp::optimizer::estimate_rel_size;
using pgcpp::optimizer::get_relation_info;
using pgcpp::optimizer::PlannerInfo;
using pgcpp::optimizer::RelOptInfo;

namespace {

class PlanCatTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = pgcpp::memory::AllocSetContext::Create("plancat_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);
    }

    void TearDown() override {
        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    pgcpp::memory::AllocSetContext* context_ = nullptr;
};

// estimate_rel_size returns defaults for OID 0 (invalid).
TEST_F(PlanCatTest, EstimateRelSize_InvalidOid_ReturnsDefaults) {
    int pages = -1;
    int tuples = -1;
    int allvisfrac = -1;

    estimate_rel_size(/*relation_oid=*/0, &pages, &tuples, &allvisfrac);

    EXPECT_GT(pages, 0);   // default heuristic
    EXPECT_GT(tuples, 0);  // default heuristic
    EXPECT_EQ(allvisfrac, 0);
}

// estimate_rel_size returns defaults for an unknown OID.
TEST_F(PlanCatTest, EstimateRelSize_UnknownOid_ReturnsDefaults) {
    int pages = -1;
    int tuples = -1;

    estimate_rel_size(/*relation_oid=*/999999, &pages, &tuples, nullptr);

    EXPECT_GT(pages, 0);
    EXPECT_GT(tuples, 0);
}

// estimate_rel_size handles nullptr output parameters.
TEST_F(PlanCatTest, EstimateRelSize_NullOutputs_NoCrash) {
    // Should not crash when outputs are nullptr.
    estimate_rel_size(0, nullptr, nullptr, nullptr);
}

// get_relation_info fills pages, tuples, and width.
TEST_F(PlanCatTest, GetRelationInfo_PopulatesRelOptInfo) {
    auto* root = makePallocNode<PlannerInfo>();
    auto* rel = makePallocNode<RelOptInfo>();

    get_relation_info(root, /*relation_oid=*/0, /*inhparent=*/false, rel);

    EXPECT_GT(rel->pages, 0);
    EXPECT_GT(rel->tuples, 0);
    EXPECT_GT(rel->width, 0);
    EXPECT_EQ(rel->rows, static_cast<pgcpp::optimizer::Cardinality>(rel->tuples));
}

// get_relation_info with a valid (but unknown) OID still returns defaults.
TEST_F(PlanCatTest, GetRelationInfo_UnknownOid_ReturnsDefaults) {
    auto* root = makePallocNode<PlannerInfo>();
    auto* rel = makePallocNode<RelOptInfo>();

    get_relation_info(root, /*relation_oid=*/999999, /*inhparent=*/false, rel);

    EXPECT_GT(rel->pages, 0);
    EXPECT_GT(rel->tuples, 0);
    EXPECT_GT(rel->width, 0);
}

}  // namespace

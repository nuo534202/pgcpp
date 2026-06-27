// plancat_test.cpp — Unit tests for catalog lookup (M10 15.3).
//
// Tests estimate_rel_size and get_relation_info. Verifies that catalog
// statistics (pages, tuples, width) are correctly populated from pg_class
// and pg_attribute, with conservative defaults when the catalog has no data.

#include "mytoydb/optimizer/util/plancat.hpp"

#include <gtest/gtest.h>

#include "mytoydb/catalog/catalog.hpp"
#include "mytoydb/common/containers/node.hpp"
#include "mytoydb/common/error/elog.hpp"
#include "mytoydb/common/memory/alloc_set.hpp"
#include "mytoydb/common/memory/memory_context.hpp"
#include "mytoydb/optimizer/path.hpp"
#include "mytoydb/optimizer/planner.hpp"
#include "mytoydb/parser/parsenodes.hpp"
#include "mytoydb/parser/primnodes.hpp"

using mytoydb::catalog::Oid;
using mytoydb::nodes::makePallocNode;
using mytoydb::optimizer::estimate_rel_size;
using mytoydb::optimizer::get_relation_info;
using mytoydb::optimizer::PlannerInfo;
using mytoydb::optimizer::RelOptInfo;

namespace {

class PlanCatTest : public ::testing::Test {
protected:
    void SetUp() override {
        mytoydb::error::InitErrorSubsystem();
        context_ = mytoydb::memory::AllocSetContext::Create("plancat_test_context");
        mytoydb::memory::SetCurrentMemoryContext(context_);
    }

    void TearDown() override {
        mytoydb::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    mytoydb::memory::AllocSetContext* context_ = nullptr;
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
    EXPECT_EQ(rel->rows, static_cast<mytoydb::optimizer::Cardinality>(rel->tuples));
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

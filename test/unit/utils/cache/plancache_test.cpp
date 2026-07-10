// plancache_test.cpp — Unit tests for the plan cache (M3/utils/cache).
//
// Tests CachedPlan and CachedPlanSource: plan caching, validity tracking,
// and re-planning after catalog invalidation.

#include "utils/cache/plancache.hpp"

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
#include "utils/cache/inval.hpp"

using pgcpp::executor::Plan;
using pgcpp::executor::PlanType;
using pgcpp::memory::AllocSetContext;
using pgcpp::nodes::makePallocNode;
using pgcpp::parser::CmdType;
using pgcpp::parser::Const;
using pgcpp::parser::Query;
using pgcpp::parser::TargetEntry;
using pgcpp::types::Int32GetDatum;
using pgcpp::types::kInt4Oid;
using pgcpp::utils::CachedPlan;
using pgcpp::utils::CachedPlanSource;

class PlanCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        pgcpp::utils::ResetCatalogGeneration();
        context_ = AllocSetContext::Create("plancache_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);
    }

    void TearDown() override {
        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    // Helper: create a simple SELECT 1 query (produces a Result plan).
    Query* MakeSelectOneQuery() {
        auto* query = makePallocNode<Query>();
        query->command_type = CmdType::kSelect;

        auto* con = makePallocNode<Const>();
        con->consttype = kInt4Oid;
        con->constvalue = Int32GetDatum(1);
        con->constisnull = false;
        con->constbyval = true;
        con->constlen = 4;

        auto* te = makePallocNode<TargetEntry>();
        te->expr = con;
        te->resno = 1;
        te->resname = "?column?";
        query->target_list.push_back(te);

        return query;
    }

    AllocSetContext* context_ = nullptr;
};

// --- CachedPlan tests ---

TEST_F(PlanCacheTest, GetCachedPlan_BuildsPlanOnFirstCall) {
    Query* query = MakeSelectOneQuery();
    CachedPlanSource source(query, "SELECT 1");

    CachedPlan* plan = source.GetCachedPlan();
    ASSERT_NE(plan, nullptr);
    ASSERT_NE(plan->plan(), nullptr);
    EXPECT_EQ(plan->plan()->type, PlanType::kResult);
}

TEST_F(PlanCacheTest, GetCachedPlan_ReturnsSamePlanOnSecondCall) {
    Query* query = MakeSelectOneQuery();
    CachedPlanSource source(query, "SELECT 1");

    CachedPlan* plan1 = source.GetCachedPlan();
    CachedPlan* plan2 = source.GetCachedPlan();

    ASSERT_NE(plan1, nullptr);
    EXPECT_EQ(plan1, plan2);  // Same pointer — no re-planning
}

TEST_F(PlanCacheTest, CachedPlan_ValidImmediatelyAfterCreation) {
    Query* query = MakeSelectOneQuery();
    CachedPlanSource source(query, "SELECT 1");

    CachedPlan* plan = source.GetCachedPlan();
    ASSERT_NE(plan, nullptr);
    EXPECT_TRUE(plan->IsValid());
}

TEST_F(PlanCacheTest, CachedPlan_InvalidAfterDDL) {
    Query* query = MakeSelectOneQuery();
    CachedPlanSource source(query, "SELECT 1");

    CachedPlan* plan = source.GetCachedPlan();
    ASSERT_NE(plan, nullptr);
    EXPECT_TRUE(plan->IsValid());

    // Simulate DDL.
    pgcpp::utils::IncrementCatalogGeneration();
    EXPECT_FALSE(plan->IsValid());
}

TEST_F(PlanCacheTest, GetCachedPlan_ReplansAfterInvalidation) {
    Query* query = MakeSelectOneQuery();
    CachedPlanSource source(query, "SELECT 1");

    CachedPlan* plan1 = source.GetCachedPlan();
    ASSERT_NE(plan1, nullptr);
    uint64_t gen1 = plan1->generation();
    EXPECT_TRUE(plan1->IsValid());

    // Simulate DDL — plan1 is now stale (GetCachedPlan will free it on
    // next call, so we must not dereference plan1 after this point).
    pgcpp::utils::IncrementCatalogGeneration();

    // Should re-plan with a new generation.
    CachedPlan* plan2 = source.GetCachedPlan();
    ASSERT_NE(plan2, nullptr);
    EXPECT_NE(plan2->generation(), gen1);
    EXPECT_TRUE(plan2->IsValid());
}

TEST_F(PlanCacheTest, HasValidCachedPlan_TransitionsCorrectly) {
    Query* query = MakeSelectOneQuery();
    CachedPlanSource source(query, "SELECT 1");

    // Before first GetCachedPlan — no cached plan.
    EXPECT_FALSE(source.HasValidCachedPlan());

    // After first GetCachedPlan — valid.
    source.GetCachedPlan();
    EXPECT_TRUE(source.HasValidCachedPlan());

    // After DDL — invalid.
    pgcpp::utils::IncrementCatalogGeneration();
    EXPECT_FALSE(source.HasValidCachedPlan());

    // After re-plan — valid again.
    source.GetCachedPlan();
    EXPECT_TRUE(source.HasValidCachedPlan());
}

TEST_F(PlanCacheTest, QueryString_StoredCorrectly) {
    Query* query = MakeSelectOneQuery();
    CachedPlanSource source(query, "SELECT 1");
    EXPECT_EQ(source.query_string(), "SELECT 1");
}

TEST_F(PlanCacheTest, Query_AccessorReturnsStoredQuery) {
    Query* query = MakeSelectOneQuery();
    CachedPlanSource source(query, "SELECT 1");
    EXPECT_EQ(source.query(), query);
}

TEST_F(PlanCacheTest, MultipleIncrements_AllInvalidatePlan) {
    Query* query = MakeSelectOneQuery();
    CachedPlanSource source(query, "SELECT 1");

    CachedPlan* plan = source.GetCachedPlan();
    ASSERT_NE(plan, nullptr);

    pgcpp::utils::IncrementCatalogGeneration();
    EXPECT_FALSE(plan->IsValid());

    // Re-plan.
    source.GetCachedPlan();

    // Increment multiple times.
    pgcpp::utils::IncrementCatalogGeneration();
    pgcpp::utils::IncrementCatalogGeneration();
    pgcpp::utils::IncrementCatalogGeneration();

    EXPECT_FALSE(source.HasValidCachedPlan());
}

// restrictinfo_test.cpp — Unit tests for RestrictInfo construction (M10 15.3).
//
// Tests the RestrictInfo wrapper: make_restrictinfo and
// make_restrictinfos_from_quals. Verifies that qual clauses are correctly
// wrapped with optimizer metadata (required_relids, can_join).

#include "pgcpp/optimizer/util/restrictinfo.hpp"

#include <gtest/gtest.h>

#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/alloc_set.hpp"
#include "pgcpp/common/memory/memory_context.hpp"
#include "pgcpp/optimizer/path.hpp"
#include "pgcpp/optimizer/planner.hpp"
#include "pgcpp/parser/parsenodes.hpp"
#include "pgcpp/parser/primnodes.hpp"
#include "pgcpp/types/datum.hpp"

using pgcpp::nodes::makePallocNode;
using pgcpp::optimizer::make_restrictinfo;
using pgcpp::optimizer::make_restrictinfos_from_quals;
using pgcpp::optimizer::PlannerInfo;
using pgcpp::optimizer::Relids;
using pgcpp::optimizer::RestrictInfo;
using pgcpp::parser::BoolExpr;
using pgcpp::parser::BoolExprType;
using pgcpp::parser::Const;
using pgcpp::parser::Node;
using pgcpp::parser::OpExpr;
using pgcpp::parser::Var;
using pgcpp::types::Int32GetDatum;
using pgcpp::types::kBoolOid;
using pgcpp::types::kInt4Oid;

namespace {

// Operator OIDs (from bootstrap_catalog.cpp).
constexpr pgcpp::catalog::Oid kInt4EqOp = 96;   // int4 = int4
constexpr pgcpp::catalog::Oid kInt4GtOp = 521;  // int4 > int4

class RestrictInfoTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = pgcpp::memory::AllocSetContext::Create("restrictinfo_test_context");
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

    pgcpp::memory::AllocSetContext* context_ = nullptr;
};

// make_restrictinfo wraps a clause with default metadata.
TEST_F(RestrictInfoTest, MakeRestrictInfo_WrapsClause) {
    auto* root = makePallocNode<PlannerInfo>();
    Node* clause = MakeOpExpr(kInt4GtOp, MakeVar(1, 1), MakeInt4Const(5));

    Relids relids = {1};
    RestrictInfo* ri = make_restrictinfo(root, clause, /*can_join=*/false,
                                         /*pseudoconstant=*/false, relids, {}, 0);

    ASSERT_NE(ri, nullptr);
    EXPECT_EQ(ri->clause, clause);
    EXPECT_FALSE(ri->pseudoconstant);
    EXPECT_EQ(ri->norm_selec, 1.0);
    ASSERT_EQ(ri->required_relids.size(), 1u);
    EXPECT_EQ(ri->required_relids[0], 1);
}

// make_restrictinfos_from_quals handles an empty list.
TEST_F(RestrictInfoTest, MakeRestrictinfosFromQuals_EmptyList) {
    auto* root = makePallocNode<PlannerInfo>();
    std::vector<Node*> clauses;

    auto result = make_restrictinfos_from_quals(root, clauses);

    EXPECT_TRUE(result.empty());
}

// make_restrictinfos_from_quals extracts relids from a single-table OpExpr.
TEST_F(RestrictInfoTest, MakeRestrictinfosFromQuals_SingleTableOpExpr) {
    auto* root = makePallocNode<PlannerInfo>();
    Node* clause = MakeOpExpr(kInt4GtOp, MakeVar(1, 1), MakeInt4Const(5));

    auto result = make_restrictinfos_from_quals(root, {clause});

    ASSERT_EQ(result.size(), 1u);
    RestrictInfo* ri = result[0];
    ASSERT_NE(ri, nullptr);
    EXPECT_EQ(ri->clause, clause);
    EXPECT_FALSE(ri->can_join);
    ASSERT_EQ(ri->required_relids.size(), 1u);
    EXPECT_EQ(ri->required_relids[0], 1);
}

// make_restrictinfos_from_quals marks multi-table quals as can_join.
TEST_F(RestrictInfoTest, MakeRestrictinfosFromQuals_MultiTableJoinQual) {
    auto* root = makePallocNode<PlannerInfo>();
    // a > b (Var(1,1) > Var(2,1)) — references two tables.
    Node* clause = MakeOpExpr(kInt4GtOp, MakeVar(1, 1), MakeVar(2, 1));

    auto result = make_restrictinfos_from_quals(root, {clause});

    ASSERT_EQ(result.size(), 1u);
    RestrictInfo* ri = result[0];
    ASSERT_NE(ri, nullptr);
    EXPECT_TRUE(ri->can_join);
    ASSERT_EQ(ri->required_relids.size(), 2u);
    EXPECT_EQ(ri->required_relids[0], 1);
    EXPECT_EQ(ri->required_relids[1], 2);
}

// make_restrictinfos_from_quals handles multiple clauses.
TEST_F(RestrictInfoTest, MakeRestrictinfosFromQuals_MultipleClauses) {
    auto* root = makePallocNode<PlannerInfo>();
    Node* c1 = MakeOpExpr(kInt4GtOp, MakeVar(1, 1), MakeInt4Const(5));
    Node* c2 = MakeOpExpr(kInt4EqOp, MakeVar(1, 2), MakeInt4Const(10));

    auto result = make_restrictinfos_from_quals(root, {c1, c2});

    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0]->clause, c1);
    EXPECT_EQ(result[1]->clause, c2);
}

// make_restrictinfos_from_quals skips nullptr clauses.
TEST_F(RestrictInfoTest, MakeRestrictinfosFromQuals_SkipsNullClauses) {
    auto* root = makePallocNode<PlannerInfo>();
    Node* c1 = MakeOpExpr(kInt4GtOp, MakeVar(1, 1), MakeInt4Const(5));

    auto result = make_restrictinfos_from_quals(root, {nullptr, c1, nullptr});

    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0]->clause, c1);
}

}  // namespace

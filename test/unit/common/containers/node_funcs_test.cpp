#include "pgcpp/common/containers/node_funcs.hpp"

#include <gtest/gtest.h>

#include <vector>

#include "pgcpp/common/containers/node.hpp"  // makePallocNode
#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/alloc_set.hpp"
#include "pgcpp/common/memory/memory_context.hpp"
#include "pgcpp/parser/primnodes.hpp"
#include "pgcpp/types/datum.hpp"

namespace {

using mytoydb::catalog::Oid;
using mytoydb::memory::AllocSetContext;
using mytoydb::nodes::contain_aggs_of_level;
using mytoydb::nodes::contain_subplans;
using mytoydb::nodes::contain_volatile_functions;
using mytoydb::nodes::exprCollation;
using mytoydb::nodes::expression_tree_walker;
using mytoydb::nodes::exprLocation;
using mytoydb::nodes::exprType;
using mytoydb::nodes::exprTypmod;
using mytoydb::nodes::makePallocNode;
using mytoydb::nodes::NodeTag;
using mytoydb::parser::Aggref;
using mytoydb::parser::BoolExpr;
using mytoydb::parser::BoolExprType;
using mytoydb::parser::Const;
using mytoydb::parser::FuncExpr;
using mytoydb::parser::Node;
using mytoydb::parser::NullTest;
using mytoydb::parser::NullTestType;
using mytoydb::parser::OpExpr;
using mytoydb::parser::Param;
using mytoydb::parser::ParamKind;
using mytoydb::parser::RelabelType;
using mytoydb::parser::SubLink;
using mytoydb::parser::TargetEntry;
using mytoydb::parser::Var;
using mytoydb::types::kBoolOid;
using mytoydb::types::kInt4Oid;
using mytoydb::types::kInvalidOid;
using mytoydb::types::kTextOid;

constexpr Oid kTestOidA = 1001;
constexpr Oid kTestOidB = 1002;

class NodeFuncsTest : public ::testing::Test {
protected:
    void SetUp() override {
        mytoydb::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("node_funcs_test_context");
        mytoydb::memory::SetCurrentMemoryContext(context_);
    }

    void TearDown() override {
        mytoydb::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    AllocSetContext* context_ = nullptr;
};

// --- exprType --------------------------------------------------------------

TEST_F(NodeFuncsTest, ExprTypeVarReturnsVartype) {
    auto* var = makePallocNode<Var>();
    var->vartype = kTestOidA;
    EXPECT_EQ(exprType(var), kTestOidA);
}

TEST_F(NodeFuncsTest, ExprTypeConstReturnsConsttype) {
    auto* con = makePallocNode<Const>();
    con->consttype = kTestOidB;
    EXPECT_EQ(exprType(con), kTestOidB);
}

TEST_F(NodeFuncsTest, ExprTypeOpExprReturnsOpresulttype) {
    auto* op = makePallocNode<OpExpr>();
    op->opresulttype = kTestOidA;
    EXPECT_EQ(exprType(op), kTestOidA);
}

TEST_F(NodeFuncsTest, ExprTypeFuncExprReturnsFuncresulttype) {
    auto* fn = makePallocNode<FuncExpr>();
    fn->funcresulttype = kTestOidB;
    EXPECT_EQ(exprType(fn), kTestOidB);
}

TEST_F(NodeFuncsTest, ExprTypeParamReturnsParamtype) {
    auto* p = makePallocNode<Param>();
    p->paramtype = kTestOidA;
    EXPECT_EQ(exprType(p), kTestOidA);
}

TEST_F(NodeFuncsTest, ExprTypeBoolExprReturnsBoolOid) {
    auto* b = makePallocNode<BoolExpr>();
    EXPECT_EQ(exprType(b), kBoolOid);
}

TEST_F(NodeFuncsTest, ExprTypeNullTestReturnsBoolOid) {
    auto* nt = makePallocNode<NullTest>();
    EXPECT_EQ(exprType(nt), kBoolOid);
}

TEST_F(NodeFuncsTest, ExprTypeRelabelTypeReturnsResulttype) {
    auto* r = makePallocNode<RelabelType>();
    r->resulttype = kTestOidB;
    EXPECT_EQ(exprType(r), kTestOidB);
}

TEST_F(NodeFuncsTest, ExprTypeTargetEntryDelegatesToExpr) {
    auto* var = makePallocNode<Var>();
    var->vartype = kInt4Oid;
    auto* te = makePallocNode<TargetEntry>();
    te->expr = var;
    EXPECT_EQ(exprType(te), kInt4Oid);
}

TEST_F(NodeFuncsTest, ExprTypeNullptrReturnsInvalidOid) {
    EXPECT_EQ(exprType(nullptr), kInvalidOid);
}

// --- exprTypmod ------------------------------------------------------------

TEST_F(NodeFuncsTest, ExprTypmodVarReturnsVartypmod) {
    auto* var = makePallocNode<Var>();
    var->vartypmod = 42;
    EXPECT_EQ(exprTypmod(var), 42);
}

TEST_F(NodeFuncsTest, ExprTypmodVarDefaultIsMinusOne) {
    auto* var = makePallocNode<Var>();
    EXPECT_EQ(exprTypmod(var), -1);
}

TEST_F(NodeFuncsTest, ExprTypmodOpExprIsMinusOne) {
    auto* op = makePallocNode<OpExpr>();
    EXPECT_EQ(exprTypmod(op), -1);
}

TEST_F(NodeFuncsTest, ExprTypmodRelabelTypeReturnsResulttypmod) {
    auto* r = makePallocNode<RelabelType>();
    r->resulttypmod = 7;
    EXPECT_EQ(exprTypmod(r), 7);
}

TEST_F(NodeFuncsTest, ExprTypmodNullptrReturnsMinusOne) {
    EXPECT_EQ(exprTypmod(nullptr), -1);
}

// --- exprCollation ---------------------------------------------------------

TEST_F(NodeFuncsTest, ExprCollationVarReturnsVarcollid) {
    auto* var = makePallocNode<Var>();
    var->varcollid = kTestOidA;
    EXPECT_EQ(exprCollation(var), kTestOidA);
}

TEST_F(NodeFuncsTest, ExprCollationConstReturnsConstcollid) {
    auto* con = makePallocNode<Const>();
    con->constcollid = kTestOidB;
    EXPECT_EQ(exprCollation(con), kTestOidB);
}

TEST_F(NodeFuncsTest, ExprCollationOpExprReturnsOpcollid) {
    auto* op = makePallocNode<OpExpr>();
    op->opcollid = kTestOidA;
    EXPECT_EQ(exprCollation(op), kTestOidA);
}

TEST_F(NodeFuncsTest, ExprCollationBoolExprReturnsInvalidOid) {
    auto* b = makePallocNode<BoolExpr>();
    EXPECT_EQ(exprCollation(b), kInvalidOid);
}

TEST_F(NodeFuncsTest, ExprCollationNullptrReturnsInvalidOid) {
    EXPECT_EQ(exprCollation(nullptr), kInvalidOid);
}

// --- exprLocation ----------------------------------------------------------

TEST_F(NodeFuncsTest, ExprLocationVarReturnsLocation) {
    auto* var = makePallocNode<Var>();
    var->location = 17;
    EXPECT_EQ(exprLocation(var), 17);
}

TEST_F(NodeFuncsTest, ExprLocationConstDefaultIsMinusOne) {
    auto* con = makePallocNode<Const>();
    EXPECT_EQ(exprLocation(con), -1);
}

TEST_F(NodeFuncsTest, ExprLocationTargetEntryDelegatesToExpr) {
    auto* var = makePallocNode<Var>();
    var->location = 99;
    auto* te = makePallocNode<TargetEntry>();
    te->expr = var;
    EXPECT_EQ(exprLocation(te), 99);
}

TEST_F(NodeFuncsTest, ExprLocationNullptrReturnsMinusOne) {
    EXPECT_EQ(exprLocation(nullptr), -1);
}

// --- expression_tree_walker ------------------------------------------------

TEST_F(NodeFuncsTest, WalkerVisitsRootFirst) {
    auto* var = makePallocNode<Var>();
    var->location = 5;
    std::vector<Node*> visited;
    bool result = expression_tree_walker(var, [&](Node* n) {
        visited.push_back(n);
        return false;
    });
    EXPECT_FALSE(result);
    ASSERT_EQ(visited.size(), 1u);
    EXPECT_EQ(visited[0], var);
}

TEST_F(NodeFuncsTest, WalkerRecursesIntoOpExprArgs) {
    auto* left = makePallocNode<Var>();
    left->location = 1;
    auto* right = makePallocNode<Const>();
    right->location = 2;
    auto* op = makePallocNode<OpExpr>();
    op->args = {left, right};

    std::vector<Node*> visited;
    expression_tree_walker(op, [&](Node* n) {
        visited.push_back(n);
        return false;
    });
    ASSERT_EQ(visited.size(), 3u);
    EXPECT_EQ(visited[0], op);
    EXPECT_EQ(visited[1], left);
    EXPECT_EQ(visited[2], right);
}

TEST_F(NodeFuncsTest, WalkerShortCircuitsOnTrue) {
    auto* left = makePallocNode<Var>();
    auto* right = makePallocNode<Const>();
    auto* op = makePallocNode<OpExpr>();
    op->args = {left, right};

    int count = 0;
    bool result = expression_tree_walker(op, [&](Node* n) {
        ++count;
        return n == left;  // stop after visiting left
    });
    EXPECT_TRUE(result);
    EXPECT_EQ(count, 2);  // op + left, right not visited
}

TEST_F(NodeFuncsTest, WalkerNullptrReturnsFalse) {
    int count = 0;
    bool result = expression_tree_walker(nullptr, [&](Node*) {
        ++count;
        return false;
    });
    EXPECT_FALSE(result);
    EXPECT_EQ(count, 0);
}

TEST_F(NodeFuncsTest, WalkerRecursesIntoBoolExprArgs) {
    auto* a = makePallocNode<Var>();
    auto* b = makePallocNode<Var>();
    auto* bool_expr = makePallocNode<BoolExpr>();
    bool_expr->boolop = BoolExprType::kAnd;
    bool_expr->args = {a, b};

    int count = 0;
    expression_tree_walker(bool_expr, [&](Node*) {
        ++count;
        return false;
    });
    EXPECT_EQ(count, 3);  // bool_expr + a + b
}

TEST_F(NodeFuncsTest, WalkerRecursesIntoNullTestArg) {
    auto* arg = makePallocNode<Var>();
    auto* nt = makePallocNode<NullTest>();
    nt->arg = arg;
    nt->nulltesttype = NullTestType::kIsNull;

    int count = 0;
    expression_tree_walker(nt, [&](Node*) {
        ++count;
        return false;
    });
    EXPECT_EQ(count, 2);  // nt + arg
}

// --- contain_aggs_of_level -------------------------------------------------

TEST_F(NodeFuncsTest, ContainAggsOfLevelFindsAggref) {
    auto* agg = makePallocNode<Aggref>();
    agg->agglevelsup = 0;
    EXPECT_TRUE(contain_aggs_of_level(agg, 0));
}

TEST_F(NodeFuncsTest, ContainAggsOfLevelWrongLevel) {
    auto* agg = makePallocNode<Aggref>();
    agg->agglevelsup = 1;
    EXPECT_FALSE(contain_aggs_of_level(agg, 0));
    EXPECT_TRUE(contain_aggs_of_level(agg, 1));
}

TEST_F(NodeFuncsTest, ContainAggsOfLevelNestedInOpExpr) {
    auto* agg = makePallocNode<Aggref>();
    agg->agglevelsup = 0;
    auto* var = makePallocNode<Var>();
    auto* op = makePallocNode<OpExpr>();
    op->args = {var, agg};
    EXPECT_TRUE(contain_aggs_of_level(op, 0));
}

TEST_F(NodeFuncsTest, ContainAggsOfLevelNoAgg) {
    auto* var = makePallocNode<Var>();
    auto* con = makePallocNode<Const>();
    auto* op = makePallocNode<OpExpr>();
    op->args = {var, con};
    EXPECT_FALSE(contain_aggs_of_level(op, 0));
}

TEST_F(NodeFuncsTest, ContainAggsOfLevelNullptr) {
    EXPECT_FALSE(contain_aggs_of_level(nullptr, 0));
}

// --- contain_subplans ------------------------------------------------------

TEST_F(NodeFuncsTest, ContainSubplansFindsSubLink) {
    auto* sl = makePallocNode<SubLink>();
    EXPECT_TRUE(contain_subplans(sl));
}

TEST_F(NodeFuncsTest, ContainSubplansNestedInOpExpr) {
    auto* var = makePallocNode<Var>();
    auto* sl = makePallocNode<SubLink>();
    auto* op = makePallocNode<OpExpr>();
    op->args = {var, sl};
    EXPECT_TRUE(contain_subplans(op));
}

TEST_F(NodeFuncsTest, ContainSubplansNone) {
    auto* var = makePallocNode<Var>();
    EXPECT_FALSE(contain_subplans(var));
}

// --- contain_volatile_functions (stub) -------------------------------------

TEST_F(NodeFuncsTest, ContainVolatileFunctionsStubReturnsFalse) {
    auto* fn = makePallocNode<FuncExpr>();
    EXPECT_FALSE(contain_volatile_functions(fn));
    EXPECT_FALSE(contain_volatile_functions(nullptr));
}

}  // namespace

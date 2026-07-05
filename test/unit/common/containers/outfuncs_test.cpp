// outfuncs_test.cpp — round-trip tests for node serialization.
//
// Tests that nodeToString() → stringToNode() preserves node equality
// for all core node types (Var, Const, OpExpr, FuncExpr, BoolExpr,
// TargetEntry, NullTest, BooleanTest, RangeTblEntry, Query, Value).
#include "common/containers/outfuncs.hpp"
#include "common/containers/readfuncs.hpp"

#include <gtest/gtest.h>

#include <string>

#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "parser/parsenodes.hpp"
#include "parser/primnodes.hpp"

namespace {

using pgcpp::memory::AllocSetContext;
using pgcpp::nodes::equal;
using pgcpp::nodes::makePallocNode;
using pgcpp::nodes::Node;
using pgcpp::nodes::nodeToStdString;
using pgcpp::nodes::stringToNode;
using pgcpp::nodes::Value;
using pgcpp::parser::BoolExpr;
using pgcpp::parser::BoolExprType;
using pgcpp::parser::BooleanTest;
using pgcpp::parser::BoolTestType;
using pgcpp::parser::Const;
using pgcpp::parser::FuncExpr;
using pgcpp::parser::NullTest;
using pgcpp::parser::NullTestType;
using pgcpp::parser::OpExpr;
using pgcpp::parser::Param;
using pgcpp::parser::ParamKind;
using pgcpp::parser::Query;
using pgcpp::parser::RangeTblEntry;
using pgcpp::parser::RTEKind;
using pgcpp::parser::TargetEntry;
using pgcpp::parser::Var;

class OutFuncsTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("outfuncs_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);
    }

    void TearDown() override {
        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        delete context_;
    }

    // Round-trip helper: serialize → deserialize → compare with equal().
    bool roundTripEqual(const Node* original) {
        std::string serialized = nodeToStdString(original);
        Node* deserialized = stringToNode(serialized.c_str());
        if (deserialized == nullptr) return false;
        return equal(original, deserialized);
    }

    // Get the serialized string for inspection.
    std::string serialize(const Node* node) {
        return nodeToStdString(node);
    }

    pgcpp::memory::MemoryContext* context_ = nullptr;
};

// --- Var ---

TEST_F(OutFuncsTest, VarRoundTrip) {
    auto* var = makePallocNode<Var>();
    var->varno = 1;
    var->varattno = 2;
    var->vartype = 23;
    var->vartypmod = -1;
    var->varcollid = 0;
    var->varlevelsup = 0;
    var->varnosyn = 1;
    var->varattnosyn = 2;
    var->location = 42;

    EXPECT_TRUE(roundTripEqual(var));
}

TEST_F(OutFuncsTest, VarSerializationFormat) {
    auto* var = makePallocNode<Var>();
    var->varno = 3;
    var->varattno = 5;
    var->vartype = 25;
    var->location = -1;

    std::string s = serialize(var);
    EXPECT_NE(s.find("(VAR"), std::string::npos);
    EXPECT_NE(s.find(":varno 3"), std::string::npos);
    EXPECT_NE(s.find(":varattno 5"), std::string::npos);
    EXPECT_NE(s.find(":vartype 25"), std::string::npos);
}

// --- Const ---

TEST_F(OutFuncsTest, ConstRoundTrip) {
    auto* con = makePallocNode<Const>();
    con->consttype = 23;
    con->consttypmod = -1;
    con->constcollid = 0;
    con->constlen = 4;
    con->constvalue = 42;
    con->constisnull = false;
    con->constbyval = true;
    con->location = 10;

    EXPECT_TRUE(roundTripEqual(con));
}

TEST_F(OutFuncsTest, ConstNullRoundTrip) {
    auto* con = makePallocNode<Const>();
    con->consttype = 25;
    con->constisnull = true;
    con->location = -1;

    EXPECT_TRUE(roundTripEqual(con));
}

// --- Param ---

TEST_F(OutFuncsTest, ParamRoundTrip) {
    auto* param = makePallocNode<Param>();
    param->paramkind = ParamKind::kExtern;
    param->paramid = 1;
    param->paramtype = 23;
    param->paramtypmod = -1;
    param->paramcollid = 0;
    param->location = 5;

    EXPECT_TRUE(roundTripEqual(param));
}

// --- OpExpr (with args) ---

TEST_F(OutFuncsTest, OpExprRoundTrip) {
    auto* var = makePallocNode<Var>();
    var->varno = 1;
    var->varattno = 1;
    var->vartype = 23;

    auto* con = makePallocNode<Const>();
    con->consttype = 23;
    con->constvalue = 100;
    con->constbyval = true;

    auto* op = makePallocNode<OpExpr>();
    op->opno = 415;  // int4eq
    op->opfuncid = 65;
    op->opresulttype = 16;  // bool
    op->opretset = false;
    op->opcollid = 0;
    op->inputcollid = 0;
    op->args = {var, con};
    op->location = 20;

    EXPECT_TRUE(roundTripEqual(op));
}

// --- FuncExpr (with args) ---

TEST_F(OutFuncsTest, FuncExprRoundTrip) {
    auto* var = makePallocNode<Var>();
    var->varno = 1;
    var->varattno = 1;

    auto* func = makePallocNode<FuncExpr>();
    func->funcid = 200;  // int4eq
    func->funcresulttype = 16;
    func->funcretset = false;
    func->funcvariadic = false;
    func->args = {var};
    func->location = -1;

    EXPECT_TRUE(roundTripEqual(func));
}

// --- BoolExpr (with args) ---

TEST_F(OutFuncsTest, BoolExprRoundTrip) {
    auto* var1 = makePallocNode<Var>();
    var1->varno = 1;
    var1->varattno = 1;

    auto* var2 = makePallocNode<Var>();
    var2->varno = 1;
    var2->varattno = 2;

    auto* bool_expr = makePallocNode<BoolExpr>();
    bool_expr->boolop = BoolExprType::kAnd;
    bool_expr->args = {var1, var2};
    bool_expr->location = -1;

    EXPECT_TRUE(roundTripEqual(bool_expr));
}

// --- NullTest ---

TEST_F(OutFuncsTest, NullTestRoundTrip) {
    auto* var = makePallocNode<Var>();
    var->varno = 1;
    var->varattno = 1;

    auto* nt = makePallocNode<NullTest>();
    nt->arg = var;
    nt->nulltesttype = NullTestType::kIsNull;
    nt->argisrow = false;
    nt->location = -1;

    EXPECT_TRUE(roundTripEqual(nt));
}

// --- BooleanTest ---

TEST_F(OutFuncsTest, BooleanTestRoundTrip) {
    auto* var = makePallocNode<Var>();
    var->varno = 1;
    var->varattno = 1;

    auto* bt = makePallocNode<BooleanTest>();
    bt->arg = var;
    bt->booltesttype = BoolTestType::kIsTrue;
    bt->location = -1;

    EXPECT_TRUE(roundTripEqual(bt));
}

// --- TargetEntry (with nested expr) ---

TEST_F(OutFuncsTest, TargetEntryRoundTrip) {
    auto* var = makePallocNode<Var>();
    var->varno = 1;
    var->varattno = 2;
    var->vartype = 25;

    auto* te = makePallocNode<TargetEntry>();
    te->expr = var;
    te->resno = 1;
    te->resname = "col1";
    te->ressortgroupref = 0;
    te->resorigtbl = 16384;
    te->resorigcol = 2;
    te->resjunk = false;

    EXPECT_TRUE(roundTripEqual(te));
}

TEST_F(OutFuncsTest, TargetEntryEmptyNameRoundTrip) {
    auto* con = makePallocNode<Const>();
    con->consttype = 23;
    con->constvalue = 1;

    auto* te = makePallocNode<TargetEntry>();
    te->expr = con;
    te->resno = 1;
    te->resname = "";  // empty name → serialized as <>

    EXPECT_TRUE(roundTripEqual(te));
}

// --- RangeTblEntry ---

TEST_F(OutFuncsTest, RangeTblEntryRoundTrip) {
    auto* rte = makePallocNode<RangeTblEntry>();
    rte->rtekind = RTEKind::kRelation;
    rte->relid = 16384;
    rte->relkind = 'r';
    rte->rellockmode = 1;
    rte->security_barrier = false;

    EXPECT_TRUE(roundTripEqual(rte));
}

// --- Query (with nested rtable + target_list) ---

TEST_F(OutFuncsTest, QueryRoundTrip) {
    auto* rte = makePallocNode<RangeTblEntry>();
    rte->rtekind = RTEKind::kRelation;
    rte->relid = 16384;
    rte->relkind = 'r';

    auto* var = makePallocNode<Var>();
    var->varno = 1;
    var->varattno = 1;
    var->vartype = 23;

    auto* te = makePallocNode<TargetEntry>();
    te->expr = var;
    te->resno = 1;
    te->resname = "id";

    auto* query = makePallocNode<Query>();
    query->command_type = pgcpp::parser::CmdType::kSelect;
    query->can_set_tag = true;
    query->rtable = {rte};
    query->target_list = {te};

    EXPECT_TRUE(roundTripEqual(query));
}

// --- Value types ---

TEST_F(OutFuncsTest, ValueIntegerRoundTrip) {
    auto* val = makePallocNode<Value>(static_cast<int64_t>(42));
    EXPECT_TRUE(roundTripEqual(val));
}

TEST_F(OutFuncsTest, ValueStringRoundTrip) {
    auto* val = makePallocNode<Value>(std::string("hello"), true);
    EXPECT_TRUE(roundTripEqual(val));
}

TEST_F(OutFuncsTest, ValueNullRoundTrip) {
    auto* val = makePallocNode<Value>();
    EXPECT_TRUE(roundTripEqual(val));
}

// --- Null node handling ---

TEST_F(OutFuncsTest, NullNodeToString) {
    EXPECT_EQ(nodeToStdString(nullptr), "");
    EXPECT_EQ(stringToNode(nullptr), nullptr);
    EXPECT_EQ(stringToNode(""), nullptr);
}

// --- Deeply nested structure ---

TEST_F(OutFuncsTest, DeepNestedRoundTrip) {
    // Build: BoolExpr(AND, [OpExpr(=, [Var, Const]), NullTest(IS_NULL, [Var])])
    auto* var1 = makePallocNode<Var>();
    var1->varno = 1;
    var1->varattno = 1;
    var1->vartype = 23;

    auto* con = makePallocNode<Const>();
    con->consttype = 23;
    con->constvalue = 42;
    con->constbyval = true;

    auto* op = makePallocNode<OpExpr>();
    op->opno = 415;
    op->opresulttype = 16;
    op->args = {var1, con};

    auto* var2 = makePallocNode<Var>();
    var2->varno = 1;
    var2->varattno = 2;

    auto* nt = makePallocNode<NullTest>();
    nt->arg = var2;
    nt->nulltesttype = NullTestType::kIsNull;

    auto* bool_expr = makePallocNode<BoolExpr>();
    bool_expr->boolop = BoolExprType::kAnd;
    bool_expr->args = {op, nt};

    EXPECT_TRUE(roundTripEqual(bool_expr));
}

}  // namespace

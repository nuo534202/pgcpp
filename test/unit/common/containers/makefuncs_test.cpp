#include "mytoydb/common/containers/makefuncs.hpp"

#include <gtest/gtest.h>

#include <vector>

#include "mytoydb/common/error/elog.hpp"
#include "mytoydb/common/memory/alloc_set.hpp"
#include "mytoydb/common/memory/memory_context.hpp"
#include "mytoydb/parser/primnodes.hpp"
#include "mytoydb/types/datum.hpp"

namespace {

using mytoydb::catalog::Oid;
using mytoydb::memory::AllocSetContext;
using mytoydb::nodes::makeBoolExpr;
using mytoydb::nodes::makeConst;
using mytoydb::nodes::makeFuncExpr;
using mytoydb::nodes::makeNullConst;
using mytoydb::nodes::makeOpExpr;
using mytoydb::nodes::makeRelabelType;
using mytoydb::nodes::makeTargetEntry;
using mytoydb::nodes::makeVar;
using mytoydb::parser::BoolExprType;
using mytoydb::parser::CoercionForm;
using mytoydb::parser::Const;
using mytoydb::parser::FuncExpr;
using mytoydb::parser::Node;
using mytoydb::parser::OpExpr;
using mytoydb::parser::RelabelType;
using mytoydb::parser::TargetEntry;
using mytoydb::parser::Var;
using mytoydb::types::Datum;
using mytoydb::types::kBoolOid;
using mytoydb::types::kInt4Oid;
using mytoydb::types::kInvalidOid;
using mytoydb::types::kTextOid;

class MakefuncsTest : public ::testing::Test {
protected:
    void SetUp() override {
        mytoydb::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("makefuncs_test_context");
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

// --- makeVar ---------------------------------------------------------------

TEST_F(MakefuncsTest, MakeVarSimpleSetsFields) {
    Var* var = makeVar(/*varno=*/1, /*varattno=*/2, /*vartype=*/kInt4Oid);
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->varno, 1);
    EXPECT_EQ(var->varattno, 2);
    EXPECT_EQ(var->vartype, kInt4Oid);
    EXPECT_EQ(var->vartypmod, -1);
    EXPECT_EQ(var->varcollid, kInvalidOid);
    EXPECT_EQ(var->varlevelsup, 0);
    EXPECT_EQ(var->varnosyn, 1);
    EXPECT_EQ(var->varattnosyn, 2);
    EXPECT_EQ(var->location, -1);
}

TEST_F(MakefuncsTest, MakeVarFullSetsAllFields) {
    Var* var = makeVar(/*varno=*/3, /*varattno=*/4, /*vartype=*/kTextOid, /*vartypmod=*/8,
                       /*varcollid=*/100, /*varlevelsup=*/1, /*varnosyn=*/5,
                       /*varattnosyn=*/6, /*location=*/42);
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->varno, 3);
    EXPECT_EQ(var->varattno, 4);
    EXPECT_EQ(var->vartype, kTextOid);
    EXPECT_EQ(var->vartypmod, 8);
    EXPECT_EQ(var->varcollid, Oid{100});
    EXPECT_EQ(var->varlevelsup, 1);
    EXPECT_EQ(var->varnosyn, 5);
    EXPECT_EQ(var->varattnosyn, 6);
    EXPECT_EQ(var->location, 42);
}

// --- makeConst / makeNullConst ---------------------------------------------

TEST_F(MakefuncsTest, MakeConstSetsFields) {
    Datum val = mytoydb::types::Int32GetDatum(123);
    Const* con = makeConst(/*consttype=*/kInt4Oid, /*consttypmod=*/-1, /*constcollid=*/kInvalidOid,
                           /*constlen=*/4, /*constvalue=*/val, /*constisnull=*/false,
                           /*constbyval=*/true, /*location=*/7);
    ASSERT_NE(con, nullptr);
    EXPECT_EQ(con->consttype, kInt4Oid);
    EXPECT_EQ(con->consttypmod, -1);
    EXPECT_EQ(con->constcollid, kInvalidOid);
    EXPECT_EQ(con->constlen, 4);
    EXPECT_EQ(con->constvalue, val);
    EXPECT_FALSE(con->constisnull);
    EXPECT_TRUE(con->constbyval);
    EXPECT_EQ(con->location, 7);
}

TEST_F(MakefuncsTest, MakeConstNullValueZeroed) {
    Const* con = makeConst(kInt4Oid, -1, kInvalidOid, 4, Datum{999}, /*constisnull=*/true,
                           /*constbyval=*/true, -1);
    EXPECT_TRUE(con->constisnull);
    EXPECT_EQ(con->constvalue, Datum{0});  // constvalue zeroed when null
}

TEST_F(MakefuncsTest, MakeNullConstFields) {
    Const* con = makeNullConst(kInt4Oid, /*consttypmod=*/-1, /*constcollid=*/kInvalidOid,
                               /*location=*/9);
    ASSERT_NE(con, nullptr);
    EXPECT_EQ(con->consttype, kInt4Oid);
    EXPECT_TRUE(con->constisnull);
    EXPECT_EQ(con->constvalue, Datum{0});
    EXPECT_EQ(con->constlen, 0);
    EXPECT_FALSE(con->constbyval);
    EXPECT_EQ(con->location, 9);
}

// --- makeTargetEntry -------------------------------------------------------

TEST_F(MakefuncsTest, MakeTargetEntryFields) {
    Var* var = makeVar(1, 2, kInt4Oid);
    TargetEntry* te = makeTargetEntry(var, /*resno=*/3, /*resname=*/"col", /*resjunk=*/false);
    ASSERT_NE(te, nullptr);
    EXPECT_EQ(te->expr, var);
    EXPECT_EQ(te->resno, 3);
    EXPECT_EQ(te->resname, "col");
    EXPECT_FALSE(te->resjunk);
    EXPECT_EQ(te->ressortgroupref, 0);
    EXPECT_EQ(te->resorigtbl, kInvalidOid);
    EXPECT_EQ(te->resorigcol, 0);
}

TEST_F(MakefuncsTest, MakeTargetEntryJunk) {
    Var* var = makeVar(1, 1, kInt4Oid);
    TargetEntry* te = makeTargetEntry(var, /*resno=*/5, /*resname=*/"", /*resjunk=*/true);
    EXPECT_TRUE(te->resjunk);
    EXPECT_EQ(te->resname, "");
}

// --- makeOpExpr ------------------------------------------------------------

TEST_F(MakefuncsTest, MakeOpExprBinary) {
    Var* left = makeVar(1, 1, kInt4Oid);
    Const* right =
        makeConst(kInt4Oid, -1, kInvalidOid, 4, mytoydb::types::Int32GetDatum(10), false, true, -1);
    OpExpr* op = makeOpExpr(/*opno=*/415, /*opresulttype=*/kBoolOid, left, right);
    ASSERT_NE(op, nullptr);
    EXPECT_EQ(op->opno, Oid{415});
    EXPECT_EQ(op->opresulttype, kBoolOid);
    EXPECT_FALSE(op->opretset);
    EXPECT_EQ(op->opcollid, kInvalidOid);
    EXPECT_EQ(op->inputcollid, kInvalidOid);
    ASSERT_EQ(op->args.size(), 2u);
    EXPECT_EQ(op->args[0], left);
    EXPECT_EQ(op->args[1], right);
    EXPECT_EQ(op->location, -1);
}

TEST_F(MakefuncsTest, MakeOpExprFull) {
    Var* a = makeVar(1, 1, kInt4Oid);
    std::vector<Node*> args = {a};
    OpExpr* op = makeOpExpr(/*opno=*/1, /*opresulttype=*/kBoolOid, /*opretset=*/false,
                            /*opcollid=*/kInvalidOid, /*inputcollid=*/kInvalidOid, args,
                            /*location=*/11);
    ASSERT_EQ(op->args.size(), 1u);
    EXPECT_EQ(op->opno, Oid{1});
    EXPECT_EQ(op->location, 11);
}

// --- makeFuncExpr ----------------------------------------------------------

TEST_F(MakefuncsTest, MakeFuncExprFields) {
    Var* arg = makeVar(1, 1, kInt4Oid);
    std::vector<Node*> args = {arg};
    FuncExpr* fn = makeFuncExpr(/*funcid=*/100, /*funcresulttype=*/kInt4Oid, args,
                                /*funccollid=*/kInvalidOid, /*inputcollid=*/kInvalidOid,
                                /*funcretset=*/false,
                                /*funcformat=*/CoercionForm::kExplicit, /*location=*/5);
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->funcid, Oid{100});
    EXPECT_EQ(fn->funcresulttype, kInt4Oid);
    EXPECT_FALSE(fn->funcretset);
    EXPECT_FALSE(fn->funcvariadic);
    EXPECT_EQ(fn->funcformat, CoercionForm::kExplicit);
    EXPECT_EQ(fn->funccollid, kInvalidOid);
    EXPECT_EQ(fn->inputcollid, kInvalidOid);
    ASSERT_EQ(fn->args.size(), 1u);
    EXPECT_EQ(fn->args[0], arg);
    EXPECT_EQ(fn->location, 5);
}

// --- makeRelabelType -------------------------------------------------------

TEST_F(MakefuncsTest, MakeRelabelTypeFields) {
    Var* arg = makeVar(1, 1, kTextOid);
    RelabelType* r = makeRelabelType(arg, /*resulttype=*/kTextOid, /*resulttypmod=*/-1,
                                     /*resultcollid=*/kInvalidOid,
                                     /*relabelformat=*/CoercionForm::kExplicit,
                                     /*location=*/3);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->arg, arg);
    EXPECT_EQ(r->resulttype, kTextOid);
    EXPECT_EQ(r->resulttypmod, -1);
    EXPECT_EQ(r->resultcollid, kInvalidOid);
    EXPECT_EQ(r->relabelformat, CoercionForm::kExplicit);
    EXPECT_EQ(r->location, 3);
}

// --- makeBoolExpr ----------------------------------------------------------

TEST_F(MakefuncsTest, MakeBoolExprAnd) {
    Var* a = makeVar(1, 1, kBoolOid);
    Var* b = makeVar(1, 2, kBoolOid);
    std::vector<Node*> args = {a, b};
    auto* be = makeBoolExpr(BoolExprType::kAnd, args, /*location=*/12);
    ASSERT_NE(be, nullptr);
    EXPECT_EQ(be->boolop, BoolExprType::kAnd);
    ASSERT_EQ(be->args.size(), 2u);
    EXPECT_EQ(be->args[0], a);
    EXPECT_EQ(be->args[1], b);
    EXPECT_EQ(be->location, 12);
}

TEST_F(MakefuncsTest, MakeBoolExprOr) {
    std::vector<Node*> args;
    auto* be = makeBoolExpr(BoolExprType::kOr, args, -1);
    EXPECT_EQ(be->boolop, BoolExprType::kOr);
    EXPECT_TRUE(be->args.empty());
}

}  // namespace

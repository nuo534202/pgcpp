// coalesce_expr_test.cpp — Unit tests for COALESCE/NULLIF/GREATEST/LEAST.
//
// Verifies that the parser emits CoalesceExpr / MinMaxExpr / NullIfExpr
// nodes (rather than FuncCall) and that transformExprRecurse dispatches
// to transformCoalesceExpr / transformMinMaxExpr / the kNullif A_Expr
// case to construct properly typed transformed nodes.
//
// Integer-typed expressions are the deterministic path; text-typed
// outputs are non-deterministic at execution time (the executor formats
// text Datums as int4 — see expr_case.sql's NOTE), so these tests focus
// on the parse-tree shape and resolved common types.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "catalog/bootstrap_catalog.hpp"
#include "catalog/catalog.hpp"
#include "catalog/pg_attribute.hpp"
#include "catalog/pg_class.hpp"
#include "catalog/syscache.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "parser/analyze.hpp"
#include "parser/parse_expr.hpp"
#include "parser/parse_node.hpp"
#include "parser/parsenodes.hpp"
#include "parser/parser.hpp"
#include "parser/primnodes.hpp"
#include "types/datum.hpp"

using pgcpp::catalog::BootstrapCatalog;
using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_attribute;
using pgcpp::catalog::FormData_pg_class;
using pgcpp::catalog::Oid;
using pgcpp::catalog::RelKind;
using pgcpp::catalog::SetCatalog;
using pgcpp::catalog::SetSysCache;
using pgcpp::catalog::SysCache;
using pgcpp::memory::AllocSetContext;
using pgcpp::nodes::makePallocNode;
using pgcpp::nodes::makeString;
using pgcpp::nodes::Node;
using pgcpp::nodes::NodeTag;
using pgcpp::nodes::Value;
using pgcpp::parser::AConst;
using pgcpp::parser::AExpr;
using pgcpp::parser::AExprKind;
using pgcpp::parser::CoalesceExpr;
using pgcpp::parser::FromExpr;
using pgcpp::parser::make_parsestate;
using pgcpp::parser::MinMaxExpr;
using pgcpp::parser::MinMaxOp;
using pgcpp::parser::NullIfExpr;
using pgcpp::parser::parse_analyze;
using pgcpp::parser::ParseState;
using pgcpp::parser::Query;
using pgcpp::parser::raw_parser;
using pgcpp::parser::transformExprRecurse;
using pgcpp::types::kInt4Oid;
using pgcpp::types::kInt8Oid;
using pgcpp::types::kTextOid;

namespace {

// Helper to build a raw integer A_Const matching gram.yy's makeIntConst.
Node* MakeIntAConst(int64_t ival) {
    Value* v = makePallocNode<Value>(ival);
    auto* n = makePallocNode<AConst>();
    n->val = v;
    n->location = -1;
    return n;
}

class CoalesceExprTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("coalesce_expr_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        BootstrapCatalog(catalog_);

        syscache_ = new SysCache();
        SetSysCache(syscache_);
    }

    void TearDown() override {
        SetSysCache(nullptr);
        SetCatalog(nullptr);
        delete syscache_;
        delete catalog_;

        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    // Run a SQL string through raw_parser + parse_analyze and return the
    // first Query (or nullptr on parse/analyze failure).
    Query* AnalyzeSingle(const char* sql) {
        auto stmts = raw_parser(sql);
        if (stmts.empty())
            return nullptr;
        auto queries = parse_analyze(stmts, sql);
        if (queries.empty())
            return nullptr;
        return queries[0];
    }

    // Return the single target expression of "SELECT <expr> AS x".
    // Returns nullptr if the query shape doesn't match.
    Node* GetSingleTargetExpr(Query* qry) {
        if (qry == nullptr || qry->target_list.empty())
            return nullptr;
        using pgcpp::parser::TargetEntry;
        TargetEntry* te = static_cast<TargetEntry*>(qry->target_list[0]);
        return te ? te->expr : nullptr;
    }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
    SysCache* syscache_ = nullptr;
};

// ===========================================================================
// COALESCE — parse-tree shape and common-type resolution
// ===========================================================================

TEST_F(CoalesceExprTest, CoalesceIntegersEmitsCoalesceExpr) {
    // COALESCE(NULL, 0) — should emit a CoalesceExpr (not a FuncCall).
    Query* qry = AnalyzeSingle("SELECT COALESCE(NULL, 0) AS r");
    ASSERT_NE(qry, nullptr);
    Node* expr = GetSingleTargetExpr(qry);
    ASSERT_NE(expr, nullptr);
    EXPECT_EQ(expr->GetTag(), NodeTag::kCoalesceExpr);
}

TEST_F(CoalesceExprTest, CoalesceIntegerCommonTypeIsInt4) {
    Query* qry = AnalyzeSingle("SELECT COALESCE(1, 2, 3) AS r");
    ASSERT_NE(qry, nullptr);
    Node* expr = GetSingleTargetExpr(qry);
    ASSERT_NE(expr, nullptr);
    ASSERT_EQ(expr->GetTag(), NodeTag::kCoalesceExpr);
    auto* c = static_cast<CoalesceExpr*>(expr);
    EXPECT_EQ(c->coalescetype, kInt4Oid);
    EXPECT_EQ(c->args.size(), 3u);
}

TEST_F(CoalesceExprTest, CoalesceAllArgsPreserved) {
    Query* qry = AnalyzeSingle("SELECT COALESCE(NULL, NULL, 7) AS r");
    ASSERT_NE(qry, nullptr);
    Node* expr = GetSingleTargetExpr(qry);
    ASSERT_NE(expr, nullptr);
    ASSERT_EQ(expr->GetTag(), NodeTag::kCoalesceExpr);
    auto* c = static_cast<CoalesceExpr*>(expr);
    EXPECT_EQ(c->args.size(), 3u);
}

TEST_F(CoalesceExprTest, CoalesceStringCommonTypeIsText) {
    // All-unknown args resolve to text per select_common_type's default.
    Query* qry = AnalyzeSingle("SELECT COALESCE('a', 'b') AS r");
    ASSERT_NE(qry, nullptr);
    Node* expr = GetSingleTargetExpr(qry);
    ASSERT_NE(expr, nullptr);
    ASSERT_EQ(expr->GetTag(), NodeTag::kCoalesceExpr);
    auto* c = static_cast<CoalesceExpr*>(expr);
    EXPECT_EQ(c->coalescetype, kTextOid);
}

// ===========================================================================
// NULLIF — parse-tree shape and OpExpr→NullIfExpr conversion
// ===========================================================================

TEST_F(CoalesceExprTest, NullifIntegersEmitsNullIfExpr) {
    // NULLIF(1, 2) — should emit a NullIfExpr (not an OpExpr).
    Query* qry = AnalyzeSingle("SELECT NULLIF(1, 2) AS r");
    ASSERT_NE(qry, nullptr);
    Node* expr = GetSingleTargetExpr(qry);
    ASSERT_NE(expr, nullptr);
    EXPECT_EQ(expr->GetTag(), NodeTag::kNullIfExpr);
}

TEST_F(CoalesceExprTest, NullifCarriesOperatorOid) {
    Query* qry = AnalyzeSingle("SELECT NULLIF(1, 2) AS r");
    ASSERT_NE(qry, nullptr);
    Node* expr = GetSingleTargetExpr(qry);
    ASSERT_NE(expr, nullptr);
    ASSERT_EQ(expr->GetTag(), NodeTag::kNullIfExpr);
    auto* n = static_cast<NullIfExpr*>(expr);
    // opno should be set to the =(int4, int4) operator OID (non-zero).
    EXPECT_NE(n->opno, 0u);
    EXPECT_EQ(n->args.size(), 2u);
    // opresulttype is the type of arg1 (int4 here).
    EXPECT_EQ(n->opresulttype, kInt4Oid);
}

TEST_F(CoalesceExprTest, NullifStringEmitsNullIfExpr) {
    // NULLIF('a', 'a') — text-typed; parse-tree shape is still NullIfExpr.
    Query* qry = AnalyzeSingle("SELECT NULLIF('a', 'a') AS r");
    ASSERT_NE(qry, nullptr);
    Node* expr = GetSingleTargetExpr(qry);
    ASSERT_NE(expr, nullptr);
    EXPECT_EQ(expr->GetTag(), NodeTag::kNullIfExpr);
}

// ===========================================================================
// GREATEST / LEAST — MinMaxExpr node shape
// ===========================================================================

TEST_F(CoalesceExprTest, GreatestEmitsMinMaxExpr) {
    Query* qry = AnalyzeSingle("SELECT GREATEST(1, 2, 3) AS r");
    ASSERT_NE(qry, nullptr);
    Node* expr = GetSingleTargetExpr(qry);
    ASSERT_NE(expr, nullptr);
    ASSERT_EQ(expr->GetTag(), NodeTag::kMinMaxExpr);
    auto* m = static_cast<MinMaxExpr*>(expr);
    EXPECT_EQ(m->minmaxtype, MinMaxOp::kIsGreatest);
    EXPECT_EQ(m->args.size(), 3u);
}

TEST_F(CoalesceExprTest, LeastEmitsMinMaxExpr) {
    Query* qry = AnalyzeSingle("SELECT LEAST(1, 2, 3) AS r");
    ASSERT_NE(qry, nullptr);
    Node* expr = GetSingleTargetExpr(qry);
    ASSERT_NE(expr, nullptr);
    ASSERT_EQ(expr->GetTag(), NodeTag::kMinMaxExpr);
    auto* m = static_cast<MinMaxExpr*>(expr);
    EXPECT_EQ(m->minmaxtype, MinMaxOp::kIsLeast);
    EXPECT_EQ(m->args.size(), 3u);
}

TEST_F(CoalesceExprTest, GreatestSkipsNullArgsAtTransform) {
    // NULL args are kept in the args list; the executor skips them at
    // evaluation. Verify the args are present and unfiltered.
    Query* qry = AnalyzeSingle("SELECT GREATEST(1, NULL, 3) AS r");
    ASSERT_NE(qry, nullptr);
    Node* expr = GetSingleTargetExpr(qry);
    ASSERT_NE(expr, nullptr);
    ASSERT_EQ(expr->GetTag(), NodeTag::kMinMaxExpr);
    auto* m = static_cast<MinMaxExpr*>(expr);
    EXPECT_EQ(m->args.size(), 3u);
}

// ===========================================================================
// Direct transform tests (bypassing raw_parser)
//
// Exercises transformCoalesceExpr and transformMinMaxExpr directly to
// confirm the dispatch path that the parser relies on after the gram.yy
// change. Uses the same ParseState setup pattern as aexpr_test.cpp.
// ===========================================================================

TEST_F(CoalesceExprTest, TransformCoalesceExprDirect) {
    ParseState* pstate = make_parsestate(nullptr);
    auto* c = makePallocNode<CoalesceExpr>();
    c->args.push_back(MakeIntAConst(1));
    c->args.push_back(MakeIntAConst(2));
    c->location = 100;

    Node* result = transformExprRecurse(pstate, c);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->GetTag(), NodeTag::kCoalesceExpr);
    auto* out = static_cast<CoalesceExpr*>(result);
    EXPECT_EQ(out->coalescetype, kInt4Oid);
    EXPECT_EQ(out->args.size(), 2u);
}

TEST_F(CoalesceExprTest, TransformMinMaxExprDirect) {
    ParseState* pstate = make_parsestate(nullptr);
    auto* m = makePallocNode<MinMaxExpr>();
    m->minmaxtype = MinMaxOp::kIsGreatest;
    m->args.push_back(MakeIntAConst(1));
    m->args.push_back(MakeIntAConst(2));
    m->args.push_back(MakeIntAConst(3));
    m->location = 100;

    Node* result = transformExprRecurse(pstate, m);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->GetTag(), NodeTag::kMinMaxExpr);
    auto* out = static_cast<MinMaxExpr*>(result);
    EXPECT_EQ(out->minmaxtype, MinMaxOp::kIsGreatest);
    EXPECT_EQ(out->args.size(), 3u);
    // inputcollid is set to the common type of the args.
    EXPECT_EQ(out->inputcollid, kInt4Oid);
}

// ===========================================================================
// NULLIF via direct A_Expr construction (kNullif kind)
// ===========================================================================

TEST_F(CoalesceExprTest, TransformNullIfAExprDirect) {
    // Construct AExpr(kind=kNullif, name="=", lexpr=1, rexpr=2) — the shape
    // the corrected gram.yy emits for NULLIF(1, 2).
    ParseState* pstate = make_parsestate(nullptr);
    auto* a = makePallocNode<AExpr>();
    a->kind = AExprKind::kNullif;
    a->name.push_back(makeString(std::string("=")));
    a->lexpr = MakeIntAConst(1);
    a->rexpr = MakeIntAConst(2);
    a->location = 100;

    Node* result = transformExprRecurse(pstate, a);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->GetTag(), NodeTag::kNullIfExpr);
    auto* n = static_cast<NullIfExpr*>(result);
    EXPECT_NE(n->opno, 0u);
    EXPECT_EQ(n->opresulttype, kInt4Oid);
    EXPECT_EQ(n->args.size(), 2u);
}

// ===========================================================================
// Regression: ensure these expressions don't fall through to FuncCall
// (the pre-fix behavior produced "function does not exist" errors).
// ===========================================================================

TEST_F(CoalesceExprTest, CoalesceDoesNotProduceFuncCall) {
    Query* qry = AnalyzeSingle("SELECT COALESCE(NULL, 0) AS r");
    ASSERT_NE(qry, nullptr);
    Node* expr = GetSingleTargetExpr(qry);
    ASSERT_NE(expr, nullptr);
    // The pre-fix gram.yy produced a FuncCall that parse_func.cpp could
    // not resolve (no matching pg_proc entry for "coalesce(any, any)").
    // After the fix, the parse tree carries a CoalesceExpr directly.
    EXPECT_NE(expr->GetTag(), NodeTag::kFuncCall);
    EXPECT_NE(expr->GetTag(), NodeTag::kFuncExpr);
}

TEST_F(CoalesceExprTest, NullifDoesNotProduceFuncCall) {
    Query* qry = AnalyzeSingle("SELECT NULLIF(1, 2) AS r");
    ASSERT_NE(qry, nullptr);
    Node* expr = GetSingleTargetExpr(qry);
    ASSERT_NE(expr, nullptr);
    EXPECT_NE(expr->GetTag(), NodeTag::kFuncCall);
    EXPECT_NE(expr->GetTag(), NodeTag::kFuncExpr);
}

TEST_F(CoalesceExprTest, GreatestDoesNotProduceFuncCall) {
    Query* qry = AnalyzeSingle("SELECT GREATEST(1, 2, 3) AS r");
    ASSERT_NE(qry, nullptr);
    Node* expr = GetSingleTargetExpr(qry);
    ASSERT_NE(expr, nullptr);
    EXPECT_NE(expr->GetTag(), NodeTag::kFuncCall);
    EXPECT_NE(expr->GetTag(), NodeTag::kFuncExpr);
}

TEST_F(CoalesceExprTest, LeastDoesNotProduceFuncCall) {
    Query* qry = AnalyzeSingle("SELECT LEAST(1, 2, 3) AS r");
    ASSERT_NE(qry, nullptr);
    Node* expr = GetSingleTargetExpr(qry);
    ASSERT_NE(expr, nullptr);
    EXPECT_NE(expr->GetTag(), NodeTag::kFuncCall);
    EXPECT_NE(expr->GetTag(), NodeTag::kFuncExpr);
}

}  // namespace

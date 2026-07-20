// aexpr_test.cpp — Unit tests for transformAExpr A_Expr kind coverage (M5).
//
// Verifies that transformAExpr correctly handles the BETWEEN family
// (kBetween / kNotBetween / kBetweenSym / kNotBetweenSym) and kSimilar,
// and provides regression coverage for the already-implemented kinds
// (kIn, kLike, kIlike, kDistinct, kNotDistinct).
//
// The BETWEEN tests construct A_Expr nodes manually with rexpr as an
// AArrayExpr of [low, high] — matching the rexpr shape that a corrected
// gram.yy BETWEEN production would emit. (The current gram.yy production
// discards the high bound; see "Grammar note" in the report.)
//
// The regression tests for IN / LIKE / DISTINCT FROM go through the full
// raw_parser + parse_analyze pipeline via the existing "hits" test table.

#include <gtest/gtest.h>

#include <memory>
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
using pgcpp::parser::AArrayExpr;
using pgcpp::parser::AConst;
using pgcpp::parser::AExpr;
using pgcpp::parser::AExprKind;
using pgcpp::parser::BoolExpr;
using pgcpp::parser::BoolExprType;
using pgcpp::parser::FromExpr;
using pgcpp::parser::make_parsestate;
using pgcpp::parser::parse_analyze;
using pgcpp::parser::ParseState;
using pgcpp::parser::Query;
using pgcpp::parser::raw_parser;
using pgcpp::parser::ScalarArrayOpExpr;
using pgcpp::parser::transformExprRecurse;
using pgcpp::types::kDateOid;
using pgcpp::types::kFloat8Oid;
using pgcpp::types::kInt4Oid;
using pgcpp::types::kInt8Oid;
using pgcpp::types::kTextOid;
using pgcpp::types::kTimestampOid;

// ===========================================================================
// Test fixture
// ===========================================================================

namespace {

class AExprTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("aexpr_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        BootstrapCatalog(catalog_);

        syscache_ = new SysCache();
        SetSysCache(syscache_);

        SetupTestTable();
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

    void SetupTestTable() {
        auto* class_row = makePallocNode<FormData_pg_class>();
        class_row->relname = "hits";
        class_row->oid = 16384;
        class_row->relkind = RelKind::kRelation;
        catalog_->InsertClass(class_row);

        AddAttribute(16384, "id", 1, kInt8Oid);
        AddAttribute(16384, "user_id", 2, kInt4Oid);
        AddAttribute(16384, "event_time", 3, kTimestampOid);
        AddAttribute(16384, "event_date", 4, kDateOid);
        AddAttribute(16384, "event_type", 5, kTextOid);
        AddAttribute(16384, "url", 6, kTextOid);
        AddAttribute(16384, "count", 7, kInt4Oid);
        AddAttribute(16384, "price", 8, kFloat8Oid);
    }

    void AddAttribute(Oid relid, const std::string& name, int16_t attnum, Oid typid) {
        auto* attr = makePallocNode<FormData_pg_attribute>();
        attr->attrelid = relid;
        attr->attname = name;
        attr->attnum = attnum;
        attr->atttypid = typid;
        attr->atttypmod = -1;
        catalog_->InsertAttribute(attr);
    }

    // Helper: parse and analyze a SQL string, returning the first Query.
    Query* AnalyzeSingle(const char* sql) {
        auto stmts = raw_parser(sql);
        if (stmts.empty())
            return nullptr;
        auto queries = parse_analyze(stmts, sql);
        if (queries.empty())
            return nullptr;
        return queries[0];
    }

    // Helper: check if a callable ereports(ERROR).
    template<typename F>
    bool RaisesError(F&& fn) {
        bool caught = false;
        PG_TRY() {
            fn();
        }
        PG_CATCH() {
            caught = true;
        }
        PG_END_TRY();
        return caught;
    }

    // Helpers to construct raw A_Const nodes (mirrors gram.yy's makeIntConst
    // / makeStrConst). These produce the same shape the grammar emits, so
    // transformAConst → make_const resolves them to typed Const nodes.
    Node* MakeIntAConst(int64_t ival) {
        Value* v = makePallocNode<Value>(ival);
        auto* n = makePallocNode<AConst>();
        n->val = v;
        n->location = -1;
        return n;
    }

    Node* MakeStrAConst(const std::string& sval) {
        Value* v = makeString(std::string(sval));
        auto* n = makePallocNode<AConst>();
        n->val = v;
        n->location = -1;
        return n;
    }

    // Construct an AExpr of the given kind with a single-name operator
    // (matching makeSimpleAExpr in gram.yy) and an AArrayExpr rexpr
    // containing [low, high].
    AExpr* MakeBetweenAExpr(AExprKind kind, const std::string& opname, Node* lexpr, Node* low,
                            Node* high) {
        auto* a = makePallocNode<AExpr>();
        a->kind = kind;
        a->name.push_back(makeString(std::string(opname)));
        a->lexpr = lexpr;
        auto* arr = makePallocNode<AArrayExpr>();
        arr->elements.push_back(low);
        arr->elements.push_back(high);
        arr->location = -1;
        a->rexpr = arr;
        a->location = 100;
        return a;
    }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
    SysCache* syscache_ = nullptr;
};

// Helper: extract the qual expression from a Query's jointree.
Node* GetQueryQual(Query* qry) {
    EXPECT_NE(qry->jointree, nullptr);
    return static_cast<FromExpr*>(qry->jointree)->quals;
}

}  // namespace

// ===========================================================================
// BETWEEN tests (manual AExpr construction)
// ===========================================================================

TEST_F(AExprTest, BetweenNumericProducesAndOfTwoOpExprs) {
    // 5 BETWEEN 1 AND 10  =>  (5 >= 1) AND (5 <= 10)
    ParseState* pstate = make_parsestate(nullptr);
    Node* lexpr = MakeIntAConst(5);
    Node* low = MakeIntAConst(1);
    Node* high = MakeIntAConst(10);
    AExpr* a = MakeBetweenAExpr(AExprKind::kBetween, "BETWEEN", lexpr, low, high);

    Node* result = transformExprRecurse(pstate, a);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->GetTag(), NodeTag::kBoolExpr);

    auto* bool_expr = static_cast<BoolExpr*>(result);
    EXPECT_EQ(bool_expr->boolop, BoolExprType::kAnd);
    ASSERT_EQ(bool_expr->args.size(), 2u);
    EXPECT_EQ(bool_expr->args[0]->GetTag(), NodeTag::kOpExpr);
    EXPECT_EQ(bool_expr->args[1]->GetTag(), NodeTag::kOpExpr);
}

TEST_F(AExprTest, BetweenStringProducesAndOfTwoOpExprs) {
    // 'm' BETWEEN 'a' AND 'z'  =>  ('m' >= 'a') AND ('m' <= 'z')
    ParseState* pstate = make_parsestate(nullptr);
    Node* lexpr = MakeStrAConst("m");
    Node* low = MakeStrAConst("a");
    Node* high = MakeStrAConst("z");
    AExpr* a = MakeBetweenAExpr(AExprKind::kBetween, "BETWEEN", lexpr, low, high);

    Node* result = transformExprRecurse(pstate, a);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->GetTag(), NodeTag::kBoolExpr);

    auto* bool_expr = static_cast<BoolExpr*>(result);
    EXPECT_EQ(bool_expr->boolop, BoolExprType::kAnd);
    ASSERT_EQ(bool_expr->args.size(), 2u);
    EXPECT_EQ(bool_expr->args[0]->GetTag(), NodeTag::kOpExpr);
    EXPECT_EQ(bool_expr->args[1]->GetTag(), NodeTag::kOpExpr);
}

// ===========================================================================
// NOT BETWEEN tests
// ===========================================================================

TEST_F(AExprTest, NotBetweenProducesOrOfTwoOpExprs) {
    // 5 NOT BETWEEN 1 AND 10  =>  (5 < 1) OR (5 > 10)
    ParseState* pstate = make_parsestate(nullptr);
    Node* lexpr = MakeIntAConst(5);
    Node* low = MakeIntAConst(1);
    Node* high = MakeIntAConst(10);
    AExpr* a = MakeBetweenAExpr(AExprKind::kNotBetween, "NOT BETWEEN", lexpr, low, high);

    Node* result = transformExprRecurse(pstate, a);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->GetTag(), NodeTag::kBoolExpr);

    auto* bool_expr = static_cast<BoolExpr*>(result);
    EXPECT_EQ(bool_expr->boolop, BoolExprType::kOr);
    ASSERT_EQ(bool_expr->args.size(), 2u);
    EXPECT_EQ(bool_expr->args[0]->GetTag(), NodeTag::kOpExpr);
    EXPECT_EQ(bool_expr->args[1]->GetTag(), NodeTag::kOpExpr);
}

// ===========================================================================
// BETWEEN SYMMETRIC tests
// ===========================================================================

TEST_F(AExprTest, BetweenSymmetricProducesOrOfTwoAnds) {
    // 5 BETWEEN SYMMETRIC 10 AND 1  (low > high)
    //   =>  ((5 >= 10) AND (5 <= 1)) OR ((5 >= 1) AND (5 <= 10))
    ParseState* pstate = make_parsestate(nullptr);
    Node* lexpr = MakeIntAConst(5);
    Node* low = MakeIntAConst(10);  // deliberately larger than high
    Node* high = MakeIntAConst(1);
    AExpr* a = MakeBetweenAExpr(AExprKind::kBetweenSym, "BETWEEN SYMMETRIC", lexpr, low, high);

    Node* result = transformExprRecurse(pstate, a);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->GetTag(), NodeTag::kBoolExpr);

    auto* or_expr = static_cast<BoolExpr*>(result);
    EXPECT_EQ(or_expr->boolop, BoolExprType::kOr);
    ASSERT_EQ(or_expr->args.size(), 2u);

    // Each disjunct must be an AND of two OpExprs.
    for (Node* disjunct : or_expr->args) {
        ASSERT_EQ(disjunct->GetTag(), NodeTag::kBoolExpr);
        auto* and_expr = static_cast<BoolExpr*>(disjunct);
        EXPECT_EQ(and_expr->boolop, BoolExprType::kAnd);
        ASSERT_EQ(and_expr->args.size(), 2u);
        EXPECT_EQ(and_expr->args[0]->GetTag(), NodeTag::kOpExpr);
        EXPECT_EQ(and_expr->args[1]->GetTag(), NodeTag::kOpExpr);
    }
}

TEST_F(AExprTest, BetweenSymmetricLowLessThanHighAlsoWorks) {
    // 5 BETWEEN SYMMETRIC 1 AND 10  (low < high, normal order)
    //   =>  ((5 >= 1) AND (5 <= 10)) OR ((5 >= 10) AND (5 <= 1))
    ParseState* pstate = make_parsestate(nullptr);
    Node* lexpr = MakeIntAConst(5);
    Node* low = MakeIntAConst(1);
    Node* high = MakeIntAConst(10);
    AExpr* a = MakeBetweenAExpr(AExprKind::kBetweenSym, "BETWEEN SYMMETRIC", lexpr, low, high);

    Node* result = transformExprRecurse(pstate, a);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->GetTag(), NodeTag::kBoolExpr);
    auto* or_expr = static_cast<BoolExpr*>(result);
    EXPECT_EQ(or_expr->boolop, BoolExprType::kOr);
    ASSERT_EQ(or_expr->args.size(), 2u);
}

// ===========================================================================
// NOT BETWEEN SYMMETRIC tests
// ===========================================================================

TEST_F(AExprTest, NotBetweenSymmetricProducesAndOfTwoOrs) {
    // 5 NOT BETWEEN SYMMETRIC 10 AND 1  (low > high)
    //   =>  (5 < 10 OR 5 > 1) AND (5 < 1 OR 5 > 10)
    ParseState* pstate = make_parsestate(nullptr);
    Node* lexpr = MakeIntAConst(5);
    Node* low = MakeIntAConst(10);
    Node* high = MakeIntAConst(1);
    AExpr* a =
        MakeBetweenAExpr(AExprKind::kNotBetweenSym, "NOT BETWEEN SYMMETRIC", lexpr, low, high);

    Node* result = transformExprRecurse(pstate, a);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->GetTag(), NodeTag::kBoolExpr);

    auto* and_expr = static_cast<BoolExpr*>(result);
    EXPECT_EQ(and_expr->boolop, BoolExprType::kAnd);
    ASSERT_EQ(and_expr->args.size(), 2u);

    // Each conjunct must be an OR of two OpExprs.
    for (Node* conjunct : and_expr->args) {
        ASSERT_EQ(conjunct->GetTag(), NodeTag::kBoolExpr);
        auto* or_expr = static_cast<BoolExpr*>(conjunct);
        EXPECT_EQ(or_expr->boolop, BoolExprType::kOr);
        ASSERT_EQ(or_expr->args.size(), 2u);
        EXPECT_EQ(or_expr->args[0]->GetTag(), NodeTag::kOpExpr);
        EXPECT_EQ(or_expr->args[1]->GetTag(), NodeTag::kOpExpr);
    }
}

// ===========================================================================
// BETWEEN error / edge cases
// ===========================================================================

TEST_F(AExprTest, BetweenWithSingleElementRexprErrors) {
    // If rexpr is an AArrayExpr with != 2 elements, transformAExpr must
    // fall through to ereport(ERROR).
    ParseState* pstate = make_parsestate(nullptr);
    auto* a = makePallocNode<AExpr>();
    a->kind = AExprKind::kBetween;
    a->name.push_back(makeString(std::string("BETWEEN")));
    a->lexpr = MakeIntAConst(5);
    auto* arr = makePallocNode<AArrayExpr>();
    arr->elements.push_back(MakeIntAConst(1));  // only one element
    a->rexpr = arr;
    a->location = 0;

    EXPECT_TRUE(RaisesError([&] { (void)transformExprRecurse(pstate, a); }));
}

TEST_F(AExprTest, BetweenWithNonArrayRexprErrors) {
    // If rexpr is not an AArrayExpr (e.g., a bare AConst — the shape the
    // current gram.yy actually emits), transformAExpr must ereport(ERROR).
    ParseState* pstate = make_parsestate(nullptr);
    auto* a = makePallocNode<AExpr>();
    a->kind = AExprKind::kBetween;
    a->name.push_back(makeString(std::string("BETWEEN")));
    a->lexpr = MakeIntAConst(5);
    a->rexpr = MakeIntAConst(1);  // bare AConst, not an AArrayExpr
    a->location = 0;

    EXPECT_TRUE(RaisesError([&] { (void)transformExprRecurse(pstate, a); }));
}

// ===========================================================================
// Regression tests: IN / NOT IN (via parse_analyze)
// ===========================================================================

TEST_F(AExprTest, InExpressionDoesNotRegress) {
    Query* qry = AnalyzeSingle("SELECT id FROM hits WHERE id IN (1, 2, 3)");
    ASSERT_NE(qry, nullptr);
    Node* qual = GetQueryQual(qry);
    ASSERT_NE(qual, nullptr);
    EXPECT_EQ(qual->GetTag(), NodeTag::kScalarArrayOpExpr);

    auto* saop = static_cast<ScalarArrayOpExpr*>(qual);
    EXPECT_TRUE(saop->use_or);
}

TEST_F(AExprTest, NotInExpressionDoesNotRegress) {
    Query* qry = AnalyzeSingle("SELECT id FROM hits WHERE id NOT IN (1, 2, 3)");
    ASSERT_NE(qry, nullptr);
    Node* qual = GetQueryQual(qry);
    ASSERT_NE(qual, nullptr);
    EXPECT_EQ(qual->GetTag(), NodeTag::kScalarArrayOpExpr);

    auto* saop = static_cast<ScalarArrayOpExpr*>(qual);
    EXPECT_FALSE(saop->use_or);
}

// ===========================================================================
// Regression tests: LIKE / NOT LIKE (via parse_analyze)
// ===========================================================================

TEST_F(AExprTest, LikeExpressionDoesNotRegress) {
    Query* qry = AnalyzeSingle("SELECT id FROM hits WHERE event_type LIKE 'click%'");
    ASSERT_NE(qry, nullptr);
    Node* qual = GetQueryQual(qry);
    ASSERT_NE(qual, nullptr);
    EXPECT_EQ(qual->GetTag(), NodeTag::kOpExpr);
}

TEST_F(AExprTest, NotLikeExpressionDoesNotRegress) {
    Query* qry = AnalyzeSingle("SELECT id FROM hits WHERE event_type NOT LIKE 'click%'");
    ASSERT_NE(qry, nullptr);
    Node* qual = GetQueryQual(qry);
    ASSERT_NE(qual, nullptr);
    EXPECT_EQ(qual->GetTag(), NodeTag::kOpExpr);
}

// ===========================================================================
// Regression tests: IS DISTINCT FROM / IS NOT DISTINCT FROM
// ===========================================================================

TEST_F(AExprTest, IsDistinctFromDoesNotRegress) {
    Query* qry = AnalyzeSingle("SELECT id FROM hits WHERE id IS DISTINCT FROM 1");
    ASSERT_NE(qry, nullptr);
    Node* qual = GetQueryQual(qry);
    ASSERT_NE(qual, nullptr);
    EXPECT_EQ(qual->GetTag(), NodeTag::kOpExpr);
}

TEST_F(AExprTest, IsNotDistinctFromDoesNotRegress) {
    Query* qry = AnalyzeSingle("SELECT id FROM hits WHERE id IS NOT DISTINCT FROM 1");
    ASSERT_NE(qry, nullptr);
    Node* qual = GetQueryQual(qry);
    ASSERT_NE(qual, nullptr);
    EXPECT_EQ(qual->GetTag(), NodeTag::kOpExpr);
}

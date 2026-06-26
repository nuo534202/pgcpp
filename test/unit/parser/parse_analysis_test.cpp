// parse_analysis_test.cpp — Unit tests for parse analysis (M5 Task 5.3).
//
// Tests the parse_analyze() function which transforms RawStmt parse trees
// into Query nodes. Verifies that expression transformation, type coercion,
// target list expansion, and clause transformation work correctly.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "mytoydb/catalog/bootstrap_catalog.hpp"
#include "mytoydb/catalog/catalog.hpp"
#include "mytoydb/catalog/pg_attribute.hpp"
#include "mytoydb/catalog/pg_class.hpp"
#include "mytoydb/catalog/pg_type.hpp"
#include "mytoydb/catalog/syscache.hpp"
#include "mytoydb/common/containers/node.hpp"
#include "mytoydb/common/error/elog.hpp"
#include "mytoydb/common/memory/alloc_set.hpp"
#include "mytoydb/common/memory/memory_context.hpp"
#include "mytoydb/parser/analyze.hpp"
#include "mytoydb/parser/parse_coerce.hpp"
#include "mytoydb/parser/parse_func.hpp"
#include "mytoydb/parser/parse_node.hpp"
#include "mytoydb/parser/parse_oper.hpp"
#include "mytoydb/parser/parse_type.hpp"
#include "mytoydb/parser/parsenodes.hpp"
#include "mytoydb/parser/parser.hpp"
#include "mytoydb/parser/primnodes.hpp"
#include "mytoydb/types/datum.hpp"

using mytoydb::catalog::BootstrapCatalog;
using mytoydb::catalog::Catalog;
using mytoydb::catalog::FormData_pg_attribute;
using mytoydb::catalog::FormData_pg_class;
using mytoydb::catalog::FormData_pg_type;
using mytoydb::catalog::GetCatalog;
using mytoydb::catalog::GetSysCache;
using mytoydb::catalog::kInvalidOid;
using mytoydb::catalog::Oid;
using mytoydb::catalog::RelKind;
using mytoydb::catalog::RelPersistence;
using mytoydb::catalog::SetCatalog;
using mytoydb::catalog::SetSysCache;
using mytoydb::catalog::SysCache;
using mytoydb::memory::AllocSetContext;
using mytoydb::nodes::makePallocNode;
using mytoydb::nodes::Node;
using mytoydb::nodes::NodeTag;
using mytoydb::nodes::nodeTag;
using mytoydb::parser::AConst;
using mytoydb::parser::AExpr;
using mytoydb::parser::AExprKind;
using mytoydb::parser::Aggref;
using mytoydb::parser::Alias;
using mytoydb::parser::BoolExpr;
using mytoydb::parser::BoolExprType;
using mytoydb::parser::can_coerce_type;
using mytoydb::parser::CmdType;
using mytoydb::parser::CoerceViaIO;
using mytoydb::parser::CoercionContext;
using mytoydb::parser::CoercionForm;
using mytoydb::parser::ColumnRef;
using mytoydb::parser::Const;
using mytoydb::parser::exprType;
using mytoydb::parser::exprTypmod;
using mytoydb::parser::free_parsestate;
using mytoydb::parser::FromExpr;
using mytoydb::parser::FuncCall;
using mytoydb::parser::FuncExpr;
using mytoydb::parser::FuncLookupResult;
using mytoydb::parser::IsAggregateFunction;
using mytoydb::parser::IsBinaryCoercible;
using mytoydb::parser::JoinExpr;
using mytoydb::parser::lookup_operator;
using mytoydb::parser::LookupFuncName;
using mytoydb::parser::make_op;
using mytoydb::parser::make_parsestate;
using mytoydb::parser::make_scalar_array_op;
using mytoydb::parser::makeConst;
using mytoydb::parser::NullTestType;
using mytoydb::parser::OperatorResult;
using mytoydb::parser::OpExpr;
using mytoydb::parser::parse_analyze;
using mytoydb::parser::ParseExprKind;
using mytoydb::parser::ParseState;
using mytoydb::parser::Query;
using mytoydb::parser::RangeTblEntry;
using mytoydb::parser::RangeTblRef;
using mytoydb::parser::RangeVar;
using mytoydb::parser::raw_parser;
using mytoydb::parser::RawStmt;
using mytoydb::parser::RelabelType;
using mytoydb::parser::ResTarget;
using mytoydb::parser::RTEKind;
using mytoydb::parser::ScalarArrayOpExpr;
using mytoydb::parser::SelectStmt;
using mytoydb::parser::SortBy;
using mytoydb::parser::SortGroupClause;
using mytoydb::parser::TargetEntry;
using mytoydb::parser::transformStmt;
using mytoydb::parser::TypeCast;
using mytoydb::parser::TypeName;
using mytoydb::parser::typenameTypeId;
using mytoydb::parser::Var;
using mytoydb::types::Float8GetDatum;
using mytoydb::types::Int32GetDatum;
using mytoydb::types::Int64GetDatum;
using mytoydb::types::kBoolOid;
using mytoydb::types::kDateOid;
using mytoydb::types::kFloat4Oid;
using mytoydb::types::kFloat8Oid;
using mytoydb::types::kInt2Oid;
using mytoydb::types::kInt4Oid;
using mytoydb::types::kInt8Oid;
using mytoydb::types::kNumericOid;
using mytoydb::types::kTextOid;
using mytoydb::types::kTimestampOid;
using mytoydb::types::kVarcharOid;

namespace {

// Test fixture that provides a memory context and catalog for each test.
class ParseAnalysisTest : public ::testing::Test {
protected:
    void SetUp() override {
        mytoydb::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("parse_analysis_test_context");
        mytoydb::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);

        // Populate built-in operators, functions, casts, aggregates, and
        // collations so parse analysis can resolve them via the catalog.
        BootstrapCatalog(catalog_);

        syscache_ = new SysCache();
        SetSysCache(syscache_);

        // Set up a test table "hits" with some columns similar to ClickBench
        SetupTestTable();
    }

    void TearDown() override {
        SetSysCache(nullptr);
        SetCatalog(nullptr);
        delete syscache_;
        delete catalog_;

        mytoydb::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    void SetupTestTable() {
        // Create a pg_class entry for "hits"
        auto* class_row = makePallocNode<FormData_pg_class>();
        class_row->relname = "hits";
        class_row->oid = 16384;
        class_row->relkind = RelKind::kRelation;
        catalog_->InsertClass(class_row);

        // Create pg_attribute entries for the columns
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
    // Takes const char* (not const std::string&) to avoid constructing a
    // temporary std::string that would leak if parse_analyze ereports(ERROR)
    // (longjmp bypasses the temporary's destructor).
    Query* AnalyzeSingle(const char* sql) {
        auto stmts = raw_parser(sql);
        if (stmts.empty())
            return nullptr;
        auto queries = parse_analyze(stmts, sql);
        if (queries.empty())
            return nullptr;
        return queries[0];
    }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
    SysCache* syscache_ = nullptr;
};

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

}  // namespace

// ===========================================================================
// Type lookup tests (parse_type.cpp)
// ===========================================================================

TEST_F(ParseAnalysisTest, TypenameTypeIdResolvesBuiltinTypes) {
    EXPECT_EQ(typenameTypeId("int4"), kInt4Oid);
    EXPECT_EQ(typenameTypeId("integer"), kInt4Oid);
    EXPECT_EQ(typenameTypeId("int8"), kInt8Oid);
    EXPECT_EQ(typenameTypeId("bigint"), kInt8Oid);
    EXPECT_EQ(typenameTypeId("text"), kTextOid);
    EXPECT_EQ(typenameTypeId("varchar"), kVarcharOid);
    EXPECT_EQ(typenameTypeId("float8"), kFloat8Oid);
    EXPECT_EQ(typenameTypeId("date"), kDateOid);
    EXPECT_EQ(typenameTypeId("timestamp"), kTimestampOid);
    EXPECT_EQ(typenameTypeId("bool"), kBoolOid);
}

TEST_F(ParseAnalysisTest, TypenameTypeIdReturnsInvalidForUnknown) {
    EXPECT_EQ(typenameTypeId("nonexistent_type"), kInvalidOid);
}

TEST_F(ParseAnalysisTest, TypeCategoryPredicates) {
    EXPECT_TRUE(mytoydb::parser::type_is_numeric(kInt4Oid));
    EXPECT_TRUE(mytoydb::parser::type_is_numeric(kFloat8Oid));
    EXPECT_FALSE(mytoydb::parser::type_is_numeric(kTextOid));

    EXPECT_TRUE(mytoydb::parser::type_is_string(kTextOid));
    EXPECT_TRUE(mytoydb::parser::type_is_string(kVarcharOid));
    EXPECT_FALSE(mytoydb::parser::type_is_string(kInt4Oid));

    EXPECT_TRUE(mytoydb::parser::type_is_datetime(kDateOid));
    EXPECT_TRUE(mytoydb::parser::type_is_datetime(kTimestampOid));
    EXPECT_FALSE(mytoydb::parser::type_is_datetime(kInt4Oid));
}

// ===========================================================================
// Type coercion tests (parse_coerce.cpp)
// ===========================================================================

TEST_F(ParseAnalysisTest, IsBinaryCoercibleSameType) {
    EXPECT_TRUE(IsBinaryCoercible(kInt4Oid, kInt4Oid));
    EXPECT_TRUE(IsBinaryCoercible(kTextOid, kTextOid));
}

TEST_F(ParseAnalysisTest, IsBinaryCoercibleUnknownToAnything) {
    EXPECT_TRUE(IsBinaryCoercible(705, kInt4Oid));  // unknown -> int4
    EXPECT_TRUE(IsBinaryCoercible(705, kTextOid));  // unknown -> text
}

TEST_F(ParseAnalysisTest, IsBinaryCoercibleWidening) {
    EXPECT_TRUE(IsBinaryCoercible(kInt2Oid, kInt4Oid));
    EXPECT_TRUE(IsBinaryCoercible(kInt4Oid, kInt8Oid));
    EXPECT_TRUE(IsBinaryCoercible(kFloat4Oid, kFloat8Oid));
}

TEST_F(ParseAnalysisTest, IsBinaryCoercibleTextVarchar) {
    EXPECT_TRUE(IsBinaryCoercible(kTextOid, kVarcharOid));
    EXPECT_TRUE(IsBinaryCoercible(kVarcharOid, kTextOid));
}

TEST_F(ParseAnalysisTest, IsBinaryCoercibleNotCoercible) {
    EXPECT_FALSE(IsBinaryCoercible(kInt4Oid, kTextOid));
    EXPECT_FALSE(IsBinaryCoercible(kTextOid, kInt4Oid));
}

TEST_F(ParseAnalysisTest, CanCoerceTypeImplicit) {
    Oid input = kInt4Oid;
    Oid target = kFloat8Oid;
    EXPECT_TRUE(can_coerce_type(1, &input, &target, CoercionContext::kImplicit));
}

TEST_F(ParseAnalysisTest, CanCoerceTypeExplicit) {
    Oid input = kTextOid;
    Oid target = kInt4Oid;
    EXPECT_TRUE(can_coerce_type(1, &input, &target, CoercionContext::kExplicit));
}

// ===========================================================================
// exprType / exprTypmod tests
// ===========================================================================

TEST_F(ParseAnalysisTest, ExprTypeOfConst) {
    auto* con = mytoydb::parser::makeConst(kInt4Oid, -1, 0, 4, mytoydb::types::Int32GetDatum(42),
                                           false, true, -1);
    EXPECT_EQ(exprType(con), kInt4Oid);
    EXPECT_EQ(exprTypmod(con), -1);
}

TEST_F(ParseAnalysisTest, ExprTypeOfVar) {
    auto* var = mytoydb::parser::makeVar(1, 2, kInt4Oid, -1, 0, 0, -1);
    EXPECT_EQ(exprType(var), kInt4Oid);
}

TEST_F(ParseAnalysisTest, ExprTypeOfBoolExpr) {
    auto* b = mytoydb::parser::makeNode<BoolExpr>();
    b->boolop = BoolExprType::kAnd;
    EXPECT_EQ(exprType(b), kBoolOid);
}

// ===========================================================================
// ParseState management tests
// ===========================================================================

TEST_F(ParseAnalysisTest, MakeParseStateDefaults) {
    ParseState* pstate = make_parsestate(nullptr);
    ASSERT_NE(pstate, nullptr);
    EXPECT_EQ(pstate->parent_parse_state, nullptr);
    EXPECT_EQ(pstate->p_next_resno, 1);
    EXPECT_TRUE(pstate->p_resolve_unknowns);
    EXPECT_FALSE(pstate->p_has_aggs);
    EXPECT_FALSE(pstate->p_has_sub_links);
    free_parsestate(pstate);
}

TEST_F(ParseAnalysisTest, MakeParseStateWithParent) {
    ParseState* parent = make_parsestate(nullptr);
    parent->p_sourcetext = "SELECT 1";
    ParseState* child = make_parsestate(parent);
    EXPECT_EQ(child->parent_parse_state, parent);
    EXPECT_EQ(child->p_sourcetext, "SELECT 1");
    free_parsestate(child);
    free_parsestate(parent);
}

// ===========================================================================
// parse_analyze entry point tests
// ===========================================================================

TEST_F(ParseAnalysisTest, AnalyzeSelectConstant) {
    Query* qry = AnalyzeSingle("SELECT 1");
    ASSERT_NE(qry, nullptr);
    EXPECT_EQ(qry->command_type, CmdType::kSelect);
    ASSERT_FALSE(qry->target_list.empty());

    // The target list should have one TargetEntry
    Node* tle_node = qry->target_list[0];
    ASSERT_EQ(nodeTag(tle_node), NodeTag::kTargetEntry);
    auto* tle = static_cast<TargetEntry*>(tle_node);
    ASSERT_NE(tle->expr, nullptr);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kConst);
    EXPECT_EQ(exprType(tle->expr), kInt4Oid);
}

TEST_F(ParseAnalysisTest, AnalyzeSelectStringConstant) {
    Query* qry = AnalyzeSingle("SELECT 'hello'");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    Node* tle_node = qry->target_list[0];
    auto* tle = static_cast<TargetEntry*>(tle_node);
    // String constants start as unknown type
    EXPECT_EQ(exprType(tle->expr), 705u);  // unknown OID
}

TEST_F(ParseAnalysisTest, AnalyzeSelectFloatConstant) {
    Query* qry = AnalyzeSingle("SELECT 3.14");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    Node* tle_node = qry->target_list[0];
    auto* tle = static_cast<TargetEntry*>(tle_node);
    EXPECT_EQ(exprType(tle->expr), kFloat8Oid);
}

TEST_F(ParseAnalysisTest, AnalyzeSelectNullConstant) {
    Query* qry = AnalyzeSingle("SELECT NULL");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    Node* tle_node = qry->target_list[0];
    auto* tle = static_cast<TargetEntry*>(tle_node);
    auto* con = static_cast<Const*>(tle->expr);
    EXPECT_TRUE(con->constisnull);
}

TEST_F(ParseAnalysisTest, AnalyzeSelectBooleanConstant) {
    Query* qry = AnalyzeSingle("SELECT TRUE");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    Node* tle_node = qry->target_list[0];
    auto* tle = static_cast<TargetEntry*>(tle_node);
    EXPECT_EQ(exprType(tle->expr), kBoolOid);
}

// ===========================================================================
// Column reference resolution tests
// ===========================================================================

TEST_F(ParseAnalysisTest, AnalyzeSelectColumnFromTable) {
    Query* qry = AnalyzeSingle("SELECT id FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    Node* tle_node = qry->target_list[0];
    auto* tle = static_cast<TargetEntry*>(tle_node);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kVar);
    auto* var = static_cast<Var*>(tle->expr);
    EXPECT_EQ(var->varattno, 1);  // id is the first column
    EXPECT_EQ(var->vartype, kInt8Oid);
}

TEST_F(ParseAnalysisTest, AnalyzeSelectMultipleColumns) {
    Query* qry = AnalyzeSingle("SELECT id, user_id, event_time FROM hits");
    ASSERT_NE(qry, nullptr);
    EXPECT_EQ(qry->target_list.size(), 3u);

    auto* tle1 = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(tle1->resname, "id");
    EXPECT_EQ(exprType(tle1->expr), kInt8Oid);

    auto* tle2 = static_cast<TargetEntry*>(qry->target_list[1]);
    EXPECT_EQ(tle2->resname, "user_id");
    EXPECT_EQ(exprType(tle2->expr), kInt4Oid);

    auto* tle3 = static_cast<TargetEntry*>(qry->target_list[2]);
    EXPECT_EQ(tle3->resname, "event_time");
    EXPECT_EQ(exprType(tle3->expr), kTimestampOid);
}

TEST_F(ParseAnalysisTest, AnalyzeSelectStarExpansion) {
    Query* qry = AnalyzeSingle("SELECT * FROM hits");
    ASSERT_NE(qry, nullptr);
    // Should expand to all 8 columns
    EXPECT_EQ(qry->target_list.size(), 8u);
}

TEST_F(ParseAnalysisTest, AnalyzeSelectTableStarExpansion) {
    Query* qry = AnalyzeSingle("SELECT hits.* FROM hits");
    ASSERT_NE(qry, nullptr);
    EXPECT_EQ(qry->target_list.size(), 8u);
}

TEST_F(ParseAnalysisTest, AnalyzeSelectQualifiedColumn) {
    Query* qry = AnalyzeSingle("SELECT hits.id FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kVar);
    EXPECT_EQ(exprType(tle->expr), kInt8Oid);
}

TEST_F(ParseAnalysisTest, AnalyzeSelectColumnAlias) {
    Query* qry = AnalyzeSingle("SELECT id AS user_id FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(tle->resname, "user_id");
}

// ===========================================================================
// Range table entry tests
// ===========================================================================

TEST_F(ParseAnalysisTest, AnalyzeCreatesRangeTableEntry) {
    Query* qry = AnalyzeSingle("SELECT id FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_EQ(qry->rtable.size(), 1u);
    auto* rte = static_cast<RangeTblEntry*>(qry->rtable[0]);
    EXPECT_EQ(rte->rtekind, RTEKind::kRelation);
    EXPECT_EQ(rte->relid, 16384u);
}

TEST_F(ParseAnalysisTest, AnalyzeTableAlias) {
    Query* qry = AnalyzeSingle("SELECT h.id FROM hits h");
    ASSERT_NE(qry, nullptr);
    ASSERT_EQ(qry->rtable.size(), 1u);
    auto* rte = static_cast<RangeTblEntry*>(qry->rtable[0]);
    EXPECT_NE(rte->alias, nullptr);
    EXPECT_EQ(rte->alias->aliasname, "h");
}

// ===========================================================================
// Expression transformation tests
// ===========================================================================

TEST_F(ParseAnalysisTest, AnalyzeArithmeticExpression) {
    Query* qry = AnalyzeSingle("SELECT 1 + 2");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kOpExpr);
}

TEST_F(ParseAnalysisTest, AnalyzeComparisonExpression) {
    Query* qry = AnalyzeSingle("SELECT 1 = 2");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kOpExpr);
    EXPECT_EQ(exprType(tle->expr), kBoolOid);
}

TEST_F(ParseAnalysisTest, AnalyzeBooleanAndExpression) {
    Query* qry = AnalyzeSingle("SELECT TRUE AND FALSE");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kBoolExpr);
    auto* b = static_cast<BoolExpr*>(tle->expr);
    EXPECT_EQ(b->boolop, BoolExprType::kAnd);
}

TEST_F(ParseAnalysisTest, AnalyzeBooleanOrExpression) {
    Query* qry = AnalyzeSingle("SELECT TRUE OR FALSE");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kBoolExpr);
    auto* b = static_cast<BoolExpr*>(tle->expr);
    EXPECT_EQ(b->boolop, BoolExprType::kOr);
}

TEST_F(ParseAnalysisTest, AnalyzeBooleanNotExpression) {
    Query* qry = AnalyzeSingle("SELECT NOT TRUE");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kBoolExpr);
    auto* b = static_cast<BoolExpr*>(tle->expr);
    EXPECT_EQ(b->boolop, BoolExprType::kNot);
}

// ===========================================================================
// Function call tests
// ===========================================================================

TEST_F(ParseAnalysisTest, AnalyzeCountStar) {
    Query* qry = AnalyzeSingle("SELECT COUNT(*) FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kAggref);
    auto* agg = static_cast<Aggref*>(tle->expr);
    EXPECT_TRUE(agg->aggstar);
    EXPECT_EQ(exprType(agg), kInt8Oid);
    EXPECT_TRUE(qry->has_aggs);
}

TEST_F(ParseAnalysisTest, AnalyzeCountExpr) {
    Query* qry = AnalyzeSingle("SELECT COUNT(id) FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kAggref);
    auto* agg = static_cast<Aggref*>(tle->expr);
    EXPECT_FALSE(agg->aggstar);
    EXPECT_EQ(exprType(agg), kInt8Oid);
}

TEST_F(ParseAnalysisTest, AnalyzeSumExpr) {
    Query* qry = AnalyzeSingle("SELECT SUM(count) FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kAggref);
    // sum(int4) returns int8 per PostgreSQL (catalog-driven resolution).
    EXPECT_EQ(exprType(tle->expr), kInt8Oid);
}

TEST_F(ParseAnalysisTest, AnalyzeMinExpr) {
    Query* qry = AnalyzeSingle("SELECT MIN(price) FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kAggref);
}

TEST_F(ParseAnalysisTest, AnalyzeMaxExpr) {
    Query* qry = AnalyzeSingle("SELECT MAX(price) FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kAggref);
}

TEST_F(ParseAnalysisTest, AnalyzeAvgExpr) {
    Query* qry = AnalyzeSingle("SELECT AVG(price) FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kAggref);
    EXPECT_EQ(exprType(tle->expr), kFloat8Oid);
}

TEST_F(ParseAnalysisTest, AnalyzeLowerFunction) {
    Query* qry = AnalyzeSingle("SELECT LOWER(event_type) FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kFuncExpr);
    EXPECT_EQ(exprType(tle->expr), kTextOid);
}

TEST_F(ParseAnalysisTest, AnalyzeUpperFunction) {
    Query* qry = AnalyzeSingle("SELECT UPPER(event_type) FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kFuncExpr);
    EXPECT_EQ(exprType(tle->expr), kTextOid);
}

// ===========================================================================
// WHERE clause tests
// ===========================================================================

TEST_F(ParseAnalysisTest, AnalyzeWhereClause) {
    Query* qry = AnalyzeSingle("SELECT id FROM hits WHERE id = 1");
    ASSERT_NE(qry, nullptr);
    ASSERT_NE(qry->jointree, nullptr);
    auto* from_expr = static_cast<FromExpr*>(qry->jointree);
    ASSERT_NE(from_expr->quals, nullptr);
    EXPECT_EQ(nodeTag(from_expr->quals), NodeTag::kOpExpr);
}

TEST_F(ParseAnalysisTest, AnalyzeWhereWithAnd) {
    Query* qry = AnalyzeSingle("SELECT id FROM hits WHERE id = 1 AND user_id = 2");
    ASSERT_NE(qry, nullptr);
    auto* from_expr = static_cast<FromExpr*>(qry->jointree);
    ASSERT_NE(from_expr->quals, nullptr);
    EXPECT_EQ(nodeTag(from_expr->quals), NodeTag::kBoolExpr);
}

TEST_F(ParseAnalysisTest, AnalyzeWhereWithComparison) {
    Query* qry = AnalyzeSingle("SELECT id FROM hits WHERE price > 100.0");
    ASSERT_NE(qry, nullptr);
    auto* from_expr = static_cast<FromExpr*>(qry->jointree);
    ASSERT_NE(from_expr->quals, nullptr);
    EXPECT_EQ(nodeTag(from_expr->quals), NodeTag::kOpExpr);
}

// ===========================================================================
// GROUP BY / ORDER BY tests
// ===========================================================================

TEST_F(ParseAnalysisTest, AnalyzeGroupBy) {
    Query* qry = AnalyzeSingle("SELECT event_type, COUNT(*) FROM hits GROUP BY event_type");
    ASSERT_NE(qry, nullptr);
    EXPECT_FALSE(qry->group_clause.empty());
    EXPECT_TRUE(qry->has_aggs);
}

TEST_F(ParseAnalysisTest, AnalyzeOrderBy) {
    Query* qry = AnalyzeSingle("SELECT id FROM hits ORDER BY id");
    ASSERT_NE(qry, nullptr);
    EXPECT_FALSE(qry->sort_clause.empty());
}

TEST_F(ParseAnalysisTest, AnalyzeOrderByDesc) {
    Query* qry = AnalyzeSingle("SELECT id FROM hits ORDER BY id DESC");
    ASSERT_NE(qry, nullptr);
    EXPECT_FALSE(qry->sort_clause.empty());
}

TEST_F(ParseAnalysisTest, AnalyzeOrderByMultiple) {
    Query* qry = AnalyzeSingle("SELECT id FROM hits ORDER BY id, user_id");
    ASSERT_NE(qry, nullptr);
    EXPECT_EQ(qry->sort_clause.size(), 2u);
}

// ===========================================================================
// DISTINCT tests
// ===========================================================================

TEST_F(ParseAnalysisTest, AnalyzeDistinct) {
    Query* qry = AnalyzeSingle("SELECT DISTINCT event_type FROM hits");
    ASSERT_NE(qry, nullptr);
    EXPECT_FALSE(qry->distinct_clause.empty());
    EXPECT_FALSE(qry->has_distinct_on);
}

// ===========================================================================
// LIMIT tests
// ===========================================================================

TEST_F(ParseAnalysisTest, AnalyzeLimit) {
    Query* qry = AnalyzeSingle("SELECT id FROM hits LIMIT 10");
    ASSERT_NE(qry, nullptr);
    ASSERT_NE(qry->limit_count, nullptr);
}

TEST_F(ParseAnalysisTest, AnalyzeOffset) {
    Query* qry = AnalyzeSingle("SELECT id FROM hits OFFSET 5");
    ASSERT_NE(qry, nullptr);
    ASSERT_NE(qry->limit_offset, nullptr);
}

TEST_F(ParseAnalysisTest, AnalyzeLimitOffset) {
    Query* qry = AnalyzeSingle("SELECT id FROM hits LIMIT 10 OFFSET 5");
    ASSERT_NE(qry, nullptr);
    ASSERT_NE(qry->limit_count, nullptr);
    ASSERT_NE(qry->limit_offset, nullptr);
}

// ===========================================================================
// JOIN tests
// ===========================================================================

TEST_F(ParseAnalysisTest, AnalyzeJoin) {
    // Add a second table for joining
    auto* class_row = makePallocNode<FormData_pg_class>();
    class_row->relname = "users";
    class_row->oid = 16385;
    class_row->relkind = RelKind::kRelation;
    catalog_->InsertClass(class_row);
    AddAttribute(16385, "user_id", 1, kInt4Oid);
    AddAttribute(16385, "name", 2, kTextOid);

    Query* qry = AnalyzeSingle(
        "SELECT hits.id, users.name FROM hits JOIN users ON hits.user_id = users.user_id");
    ASSERT_NE(qry, nullptr);
    EXPECT_EQ(qry->rtable.size(), 2u);
}

// ===========================================================================
// HAVING clause tests
// ===========================================================================

TEST_F(ParseAnalysisTest, AnalyzeHavingClause) {
    Query* qry = AnalyzeSingle(
        "SELECT event_type, COUNT(*) FROM hits GROUP BY event_type HAVING COUNT(*) > 1");
    ASSERT_NE(qry, nullptr);
    ASSERT_NE(qry->having_qual, nullptr);
    EXPECT_TRUE(qry->has_aggs);
}

// ===========================================================================
// Type cast tests
// ===========================================================================

TEST_F(ParseAnalysisTest, AnalyzeCastExpression) {
    Query* qry = AnalyzeSingle("SELECT CAST(1 AS int8)");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(exprType(tle->expr), kInt8Oid);
}

TEST_F(ParseAnalysisTest, AnalyzeCastDoubleColon) {
    Query* qry = AnalyzeSingle("SELECT 1::int8");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(exprType(tle->expr), kInt8Oid);
}

// ===========================================================================
// IS NULL / IS NOT NULL tests
// ===========================================================================

TEST_F(ParseAnalysisTest, AnalyzeIsNull) {
    Query* qry = AnalyzeSingle("SELECT id FROM hits WHERE id IS NULL");
    ASSERT_NE(qry, nullptr);
    auto* from_expr = static_cast<FromExpr*>(qry->jointree);
    ASSERT_NE(from_expr->quals, nullptr);
    // IS NULL produces a NullTest node
    EXPECT_EQ(nodeTag(from_expr->quals), NodeTag::kNullTest);
}

TEST_F(ParseAnalysisTest, AnalyzeIsNotNull) {
    Query* qry = AnalyzeSingle("SELECT id FROM hits WHERE id IS NOT NULL");
    ASSERT_NE(qry, nullptr);
    auto* from_expr = static_cast<FromExpr*>(qry->jointree);
    ASSERT_NE(from_expr->quals, nullptr);
    EXPECT_EQ(nodeTag(from_expr->quals), NodeTag::kNullTest);
    auto* nt = static_cast<mytoydb::parser::NullTest*>(from_expr->quals);
    EXPECT_EQ(nt->nulltesttype, NullTestType::kIsNotNull);
}

// ===========================================================================
// CASE expression tests
// ===========================================================================

TEST_F(ParseAnalysisTest, AnalyzeCaseExpression) {
    Query* qry = AnalyzeSingle("SELECT CASE WHEN id > 0 THEN 1 ELSE 0 END FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kCaseExpr);
}

// ===========================================================================
// IN expression tests
// ===========================================================================

TEST_F(ParseAnalysisTest, AnalyzeInExpression) {
    Query* qry = AnalyzeSingle("SELECT id FROM hits WHERE id IN (1, 2, 3)");
    ASSERT_NE(qry, nullptr);
    auto* from_expr = static_cast<FromExpr*>(qry->jointree);
    ASSERT_NE(from_expr->quals, nullptr);
    // IN produces a ScalarArrayOpExpr
    EXPECT_EQ(nodeTag(from_expr->quals), NodeTag::kScalarArrayOpExpr);
}

// ===========================================================================
// LIKE expression tests
// ===========================================================================

TEST_F(ParseAnalysisTest, AnalyzeLikeExpression) {
    Query* qry = AnalyzeSingle("SELECT id FROM hits WHERE event_type LIKE 'click%'");
    ASSERT_NE(qry, nullptr);
    auto* from_expr = static_cast<FromExpr*>(qry->jointree);
    ASSERT_NE(from_expr->quals, nullptr);
    EXPECT_EQ(nodeTag(from_expr->quals), NodeTag::kOpExpr);
}

// ===========================================================================
// Subquery tests
// ===========================================================================

TEST_F(ParseAnalysisTest, AnalyzeScalarSubquery) {
    Query* qry = AnalyzeSingle("SELECT (SELECT COUNT(*) FROM hits)");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    // Subquery produces a SubLink
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kSubLink);
    EXPECT_TRUE(qry->has_sub_links);
}

TEST_F(ParseAnalysisTest, AnalyzeSubqueryInFrom) {
    Query* qry = AnalyzeSingle("SELECT t.id FROM (SELECT id FROM hits) t");
    ASSERT_NE(qry, nullptr);
    EXPECT_EQ(qry->rtable.size(), 1u);
    auto* rte = static_cast<RangeTblEntry*>(qry->rtable[0]);
    EXPECT_EQ(rte->rtekind, RTEKind::kSubquery);
}

// ===========================================================================
// INSERT/UPDATE/DELETE tests
// ===========================================================================

TEST_F(ParseAnalysisTest, AnalyzeInsert) {
    Query* qry = AnalyzeSingle("INSERT INTO hits (id) VALUES (1)");
    ASSERT_NE(qry, nullptr);
    EXPECT_EQ(qry->command_type, CmdType::kInsert);
    EXPECT_NE(qry->result_relation, 0);
}

TEST_F(ParseAnalysisTest, AnalyzeUpdate) {
    Query* qry = AnalyzeSingle("UPDATE hits SET count = 1 WHERE id = 1");
    ASSERT_NE(qry, nullptr);
    EXPECT_EQ(qry->command_type, CmdType::kUpdate);
    EXPECT_NE(qry->result_relation, 0);
}

TEST_F(ParseAnalysisTest, AnalyzeDelete) {
    Query* qry = AnalyzeSingle("DELETE FROM hits WHERE id = 1");
    ASSERT_NE(qry, nullptr);
    EXPECT_EQ(qry->command_type, CmdType::kDelete);
    EXPECT_NE(qry->result_relation, 0);
}

// ===========================================================================
// Complex query tests (ClickBench-like)
// ===========================================================================

TEST_F(ParseAnalysisTest, AnalyzeClickBenchLikeQuery1) {
    Query* qry = AnalyzeSingle("SELECT COUNT(*) FROM hits");
    ASSERT_NE(qry, nullptr);
    EXPECT_TRUE(qry->has_aggs);
}

TEST_F(ParseAnalysisTest, AnalyzeClickBenchLikeQuery2) {
    Query* qry = AnalyzeSingle("SELECT MIN(event_date), MAX(event_date) FROM hits");
    ASSERT_NE(qry, nullptr);
    EXPECT_TRUE(qry->has_aggs);
    EXPECT_EQ(qry->target_list.size(), 2u);
}

TEST_F(ParseAnalysisTest, AnalyzeClickBenchLikeQuery3) {
    Query* qry = AnalyzeSingle(
        "SELECT event_type, COUNT(*) AS cnt "
        "FROM hits "
        "GROUP BY event_type "
        "ORDER BY cnt DESC "
        "LIMIT 10");
    ASSERT_NE(qry, nullptr);
    EXPECT_TRUE(qry->has_aggs);
    EXPECT_FALSE(qry->group_clause.empty());
    EXPECT_FALSE(qry->sort_clause.empty());
    ASSERT_NE(qry->limit_count, nullptr);
}

TEST_F(ParseAnalysisTest, AnalyzeClickBenchLikeQuery4) {
    Query* qry = AnalyzeSingle("SELECT SUM(price) FROM hits WHERE event_date = '2020-01-01'");
    ASSERT_NE(qry, nullptr);
    EXPECT_TRUE(qry->has_aggs);
}

TEST_F(ParseAnalysisTest, AnalyzeClickBenchLikeQuery5) {
    Query* qry = AnalyzeSingle(
        "SELECT url, COUNT(*) AS cnt "
        "FROM hits "
        "WHERE event_type = 'click' "
        "GROUP BY url "
        "ORDER BY cnt DESC "
        "LIMIT 100");
    ASSERT_NE(qry, nullptr);
    EXPECT_TRUE(qry->has_aggs);
    EXPECT_FALSE(qry->group_clause.empty());
    EXPECT_FALSE(qry->sort_clause.empty());
}

// ===========================================================================
// Operator resolution tests (parse_oper.cpp — catalog-driven)
//
// These tests verify the 3-stage operator lookup algorithm:
//   1. Exact match via pg_operator
//   2. Unknown-type substitution
//   3. Candidate-based resolution with binary-coercibility
// And the common-type coercion fallback in make_op().
// ===========================================================================

namespace {

// UNKNOWNOID — PostgreSQL's OID for the "unknown" pseudo-type (705).
constexpr Oid kUnknownOid = 705;

// Helper: create a Const node of the given type with a dummy value.
Const* MakeIntConst(Oid type_oid) {
    return makeConst(type_oid, -1, 0, 4, Int32GetDatum(1), false, true, -1);
}

Const* MakeTextConst() {
    return makeConst(kTextOid, -1, 0, -1, 0, false, false, -1);
}

Const* MakeUnknownConst() {
    return makeConst(kUnknownOid, -1, 0, -2, 0, false, false, -1);
}

}  // namespace

// --- lookup_operator: exact match ---

TEST_F(ParseAnalysisTest, LookupOperatorExactMatchInt4Comparison) {
    // All comparison operators on int4 should resolve to non-zero opno.
    OperatorResult eq = lookup_operator("=", kInt4Oid, kInt4Oid);
    EXPECT_NE(eq.opno, 0u);
    EXPECT_EQ(eq.opresulttype, kBoolOid);
    EXPECT_FALSE(eq.opretset);

    OperatorResult ne = lookup_operator("<>", kInt4Oid, kInt4Oid);
    EXPECT_NE(ne.opno, 0u);
    EXPECT_EQ(ne.opresulttype, kBoolOid);

    OperatorResult lt = lookup_operator("<", kInt4Oid, kInt4Oid);
    EXPECT_NE(lt.opno, 0u);
    EXPECT_EQ(lt.opresulttype, kBoolOid);

    OperatorResult le = lookup_operator("<=", kInt4Oid, kInt4Oid);
    EXPECT_NE(le.opno, 0u);
    EXPECT_EQ(le.opresulttype, kBoolOid);

    OperatorResult gt = lookup_operator(">", kInt4Oid, kInt4Oid);
    EXPECT_NE(gt.opno, 0u);
    EXPECT_EQ(gt.opresulttype, kBoolOid);

    OperatorResult ge = lookup_operator(">=", kInt4Oid, kInt4Oid);
    EXPECT_NE(ge.opno, 0u);
    EXPECT_EQ(ge.opresulttype, kBoolOid);
}

TEST_F(ParseAnalysisTest, LookupOperatorExactMatchInt4Arithmetic) {
    OperatorResult plus = lookup_operator("+", kInt4Oid, kInt4Oid);
    EXPECT_NE(plus.opno, 0u);
    EXPECT_EQ(plus.opresulttype, kInt4Oid);

    OperatorResult minus = lookup_operator("-", kInt4Oid, kInt4Oid);
    EXPECT_NE(minus.opno, 0u);
    EXPECT_EQ(minus.opresulttype, kInt4Oid);

    OperatorResult mul = lookup_operator("*", kInt4Oid, kInt4Oid);
    EXPECT_NE(mul.opno, 0u);
    EXPECT_EQ(mul.opresulttype, kInt4Oid);

    OperatorResult div = lookup_operator("/", kInt4Oid, kInt4Oid);
    EXPECT_NE(div.opno, 0u);
    EXPECT_EQ(div.opresulttype, kInt4Oid);
}

TEST_F(ParseAnalysisTest, LookupOperatorExactMatchAllNumericTypes) {
    // int2, int8, float4, float8 all have comparison operators.
    for (Oid t : {kInt2Oid, kInt8Oid, kFloat4Oid, kFloat8Oid}) {
        OperatorResult eq = lookup_operator("=", t, t);
        EXPECT_NE(eq.opno, 0u) << "type oid " << t;
        EXPECT_EQ(eq.opresulttype, kBoolOid);
    }
}

TEST_F(ParseAnalysisTest, LookupOperatorExactMatchTextLike) {
    OperatorResult like = lookup_operator("~~", kTextOid, kTextOid);
    EXPECT_NE(like.opno, 0u);
    EXPECT_EQ(like.opresulttype, kBoolOid);

    OperatorResult nlike = lookup_operator("!~~", kTextOid, kTextOid);
    EXPECT_NE(nlike.opno, 0u);
    EXPECT_EQ(nlike.opresulttype, kBoolOid);
}

TEST_F(ParseAnalysisTest, LookupOperatorExactMatchDateTimestamp) {
    OperatorResult date_eq = lookup_operator("=", kDateOid, kDateOid);
    EXPECT_NE(date_eq.opno, 0u);
    EXPECT_EQ(date_eq.opresulttype, kBoolOid);

    OperatorResult ts_eq = lookup_operator("=", kTimestampOid, kTimestampOid);
    EXPECT_NE(ts_eq.opno, 0u);
    EXPECT_EQ(ts_eq.opresulttype, kBoolOid);

    // All 6 comparison operators for date.
    for (const char* op : {"=", "<>", "<", "<=", ">", ">="}) {
        OperatorResult r = lookup_operator(op, kDateOid, kDateOid);
        EXPECT_NE(r.opno, 0u) << "operator " << op;
    }
    // All 6 comparison operators for timestamp.
    for (const char* op : {"=", "<>", "<", "<=", ">", ">="}) {
        OperatorResult r = lookup_operator(op, kTimestampOid, kTimestampOid);
        EXPECT_NE(r.opno, 0u) << "operator " << op;
    }
}

// --- lookup_operator: unknown type substitution ---

TEST_F(ParseAnalysisTest, LookupOperatorUnknownSubstitutionLeft) {
    // left=unknown, right=int4 → should find int4 = int4.
    OperatorResult r = lookup_operator("=", kUnknownOid, kInt4Oid);
    EXPECT_NE(r.opno, 0u);
    EXPECT_EQ(r.opresulttype, kBoolOid);
}

TEST_F(ParseAnalysisTest, LookupOperatorUnknownSubstitutionRight) {
    // left=int4, right=unknown → should find int4 = int4.
    OperatorResult r = lookup_operator("=", kInt4Oid, kUnknownOid);
    EXPECT_NE(r.opno, 0u);
    EXPECT_EQ(r.opresulttype, kBoolOid);
}

TEST_F(ParseAnalysisTest, LookupOperatorBothUnknownResolvesToText) {
    // Both unknown → should resolve to text = text for comparison operators.
    OperatorResult r = lookup_operator("=", kUnknownOid, kUnknownOid);
    EXPECT_NE(r.opno, 0u);
    EXPECT_EQ(r.opresulttype, kBoolOid);
}

// --- lookup_operator: binary-coercible fallback ---

TEST_F(ParseAnalysisTest, LookupOperatorBinaryCoercibleVarchar) {
    // varchar is binary-coercible to text; text = should be found via
    // candidate-based resolution.
    OperatorResult r = lookup_operator("=", kVarcharOid, kVarcharOid);
    EXPECT_NE(r.opno, 0u);
    EXPECT_EQ(r.opresulttype, kBoolOid);
}

// --- lookup_operator: no match ---

TEST_F(ParseAnalysisTest, LookupOperatorNoMatchReturnsZeroOpno) {
    // Nonexistent operator name.
    OperatorResult r = lookup_operator("@", kInt4Oid, kInt4Oid);
    EXPECT_EQ(r.opno, 0u);
}

TEST_F(ParseAnalysisTest, LookupOperatorNoMatchForMismatchedTypes) {
    // int4 = text has no exact match and no binary-coercible path.
    OperatorResult r = lookup_operator("=", kInt4Oid, kTextOid);
    // lookup_operator doesn't do common-type coercion (only make_op does),
    // so this should return opno == 0.
    EXPECT_EQ(r.opno, 0u);
}

// --- make_op: basic operator expression construction ---

TEST_F(ParseAnalysisTest, MakeOpInt4Equality) {
    ParseState* pstate = make_parsestate(nullptr);
    Const* left = MakeIntConst(kInt4Oid);
    Const* right = MakeIntConst(kInt4Oid);

    Node* result = make_op(pstate, "=", left, right, 0);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->GetTag(), NodeTag::kOpExpr);

    auto* op = static_cast<OpExpr*>(result);
    EXPECT_NE(op->opno, 0u);
    EXPECT_EQ(op->opresulttype, kBoolOid);
    EXPECT_FALSE(op->opretset);
    ASSERT_EQ(op->args.size(), 2u);
    EXPECT_EQ(op->args[0], left);
    EXPECT_EQ(op->args[1], right);

    free_parsestate(pstate);
}

TEST_F(ParseAnalysisTest, MakeOpInt4Arithmetic) {
    ParseState* pstate = make_parsestate(nullptr);
    Const* left = MakeIntConst(kInt4Oid);
    Const* right = MakeIntConst(kInt4Oid);

    Node* result = make_op(pstate, "+", left, right, 0);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->GetTag(), NodeTag::kOpExpr);

    auto* op = static_cast<OpExpr*>(result);
    EXPECT_EQ(op->opresulttype, kInt4Oid);

    free_parsestate(pstate);
}

TEST_F(ParseAnalysisTest, MakeOpTextLike) {
    ParseState* pstate = make_parsestate(nullptr);
    Const* left = MakeTextConst();
    Const* right = MakeTextConst();

    Node* result = make_op(pstate, "~~", left, right, 0);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->GetTag(), NodeTag::kOpExpr);

    auto* op = static_cast<OpExpr*>(result);
    EXPECT_EQ(op->opresulttype, kBoolOid);

    free_parsestate(pstate);
}

// --- make_op: unknown type resolution ---

TEST_F(ParseAnalysisTest, MakeOpUnknownResolvesToText) {
    ParseState* pstate = make_parsestate(nullptr);
    Const* left = MakeUnknownConst();
    Const* right = MakeUnknownConst();

    // 'x' = 'y' → both unknown, should resolve to text = text.
    Node* result = make_op(pstate, "=", left, right, 0);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->GetTag(), NodeTag::kOpExpr);

    auto* op = static_cast<OpExpr*>(result);
    EXPECT_EQ(op->opresulttype, kBoolOid);

    free_parsestate(pstate);
}

TEST_F(ParseAnalysisTest, MakeOpUnknownAndKnownResolvesToKnown) {
    ParseState* pstate = make_parsestate(nullptr);
    Const* left = MakeUnknownConst();
    Const* right = MakeIntConst(kInt4Oid);

    // '1' = 1 → unknown = int4, should resolve to int4 = int4.
    Node* result = make_op(pstate, "=", left, right, 0);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->GetTag(), NodeTag::kOpExpr);

    auto* op = static_cast<OpExpr*>(result);
    EXPECT_EQ(op->opresulttype, kBoolOid);

    free_parsestate(pstate);
}

// --- make_op: common-type coercion ---

TEST_F(ParseAnalysisTest, MakeOpInt4AndInt8CoercesToInt8) {
    ParseState* pstate = make_parsestate(nullptr);
    Const* left = MakeIntConst(kInt4Oid);
    Const* right = makeConst(kInt8Oid, -1, 0, 8, Int64GetDatum(1), false, true, -1);

    // int4 = int8 → no exact match; should coerce int4 to int8 and find int8 =.
    Node* result = make_op(pstate, "=", left, right, 0);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->GetTag(), NodeTag::kOpExpr);

    auto* op = static_cast<OpExpr*>(result);
    EXPECT_EQ(op->opresulttype, kBoolOid);
    // The left argument should have been coerced (wrapped in RelabelType).
    EXPECT_EQ(op->args[0]->GetTag(), NodeTag::kRelabelType);

    free_parsestate(pstate);
}

TEST_F(ParseAnalysisTest, MakeOpInt4AndFloat8CoercesToFloat8) {
    ParseState* pstate = make_parsestate(nullptr);
    Const* left = MakeIntConst(kInt4Oid);
    Const* right = makeConst(kFloat8Oid, -1, 0, 8, Float8GetDatum(1.0), false, true, -1);

    // int4 + float8 → no exact match; should coerce int4 to float8 and
    // find float8 + float8.
    Node* result = make_op(pstate, "+", left, right, 0);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->GetTag(), NodeTag::kOpExpr);

    auto* op = static_cast<OpExpr*>(result);
    EXPECT_EQ(op->opresulttype, kFloat8Oid);
    // The left argument should have been coerced to float8. With constant
    // folding (matching PostgreSQL's eval_const_expressions), a Const input
    // is folded to a float8 Const; otherwise it is wrapped in RelabelType
    // or CoerceViaIO. Either way the effective type must be float8.
    Node* larg = op->args[0];
    Oid ltype = kInvalidOid;
    if (larg->GetTag() == NodeTag::kConst) {
        ltype = static_cast<Const*>(larg)->consttype;
    } else if (larg->GetTag() == NodeTag::kRelabelType) {
        ltype = static_cast<RelabelType*>(larg)->resulttype;
    } else if (larg->GetTag() == NodeTag::kCoerceViaIO) {
        ltype = static_cast<CoerceViaIO*>(larg)->resulttype;
    }
    EXPECT_EQ(ltype, kFloat8Oid);

    free_parsestate(pstate);
}

// --- make_op: error cases ---

TEST_F(ParseAnalysisTest, MakeOpNonexistentOperatorErrors) {
    ParseState* pstate = make_parsestate(nullptr);
    Const* left = MakeIntConst(kInt4Oid);
    Const* right = MakeIntConst(kInt4Oid);

    EXPECT_TRUE(RaisesError([&] { make_op(pstate, "@", left, right, 0); }));

    free_parsestate(pstate);
}

// --- make_scalar_array_op: IN expression ---

TEST_F(ParseAnalysisTest, MakeScalarArrayOpInt4In) {
    ParseState* pstate = make_parsestate(nullptr);
    Const* left = MakeIntConst(kInt4Oid);
    Const* right = MakeIntConst(kInt4Oid);  // simplified: array as scalar

    Node* result = make_scalar_array_op(pstate, "=", true, left, right, 0);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->GetTag(), NodeTag::kScalarArrayOpExpr);

    auto* saop = static_cast<ScalarArrayOpExpr*>(result);
    EXPECT_NE(saop->opno, 0u);
    EXPECT_TRUE(saop->use_or);
    ASSERT_EQ(saop->args.size(), 2u);

    free_parsestate(pstate);
}

TEST_F(ParseAnalysisTest, MakeScalarArrayOpTextIn) {
    ParseState* pstate = make_parsestate(nullptr);
    Const* left = MakeTextConst();
    Const* right = MakeTextConst();

    Node* result = make_scalar_array_op(pstate, "=", true, left, right, 0);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->GetTag(), NodeTag::kScalarArrayOpExpr);

    auto* saop = static_cast<ScalarArrayOpExpr*>(result);
    EXPECT_NE(saop->opno, 0u);
    EXPECT_TRUE(saop->use_or);

    free_parsestate(pstate);
}

TEST_F(ParseAnalysisTest, MakeScalarArrayOpUnknownResolvesToText) {
    ParseState* pstate = make_parsestate(nullptr);
    Const* left = MakeUnknownConst();
    Const* right = MakeUnknownConst();

    // Unknown IN (...) → should resolve to text =.
    Node* result = make_scalar_array_op(pstate, "=", true, left, right, 0);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->GetTag(), NodeTag::kScalarArrayOpExpr);

    free_parsestate(pstate);
}

TEST_F(ParseAnalysisTest, MakeScalarArrayOpUseOrFalseForAll) {
    ParseState* pstate = make_parsestate(nullptr);
    Const* left = MakeIntConst(kInt4Oid);
    Const* right = MakeIntConst(kInt4Oid);

    Node* result = make_scalar_array_op(pstate, "=", false, left, right, 0);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->GetTag(), NodeTag::kScalarArrayOpExpr);

    auto* saop = static_cast<ScalarArrayOpExpr*>(result);
    EXPECT_FALSE(saop->use_or);

    free_parsestate(pstate);
}

TEST_F(ParseAnalysisTest, MakeScalarArrayOpNonexistentOperatorErrors) {
    ParseState* pstate = make_parsestate(nullptr);
    Const* left = MakeIntConst(kInt4Oid);
    Const* right = MakeIntConst(kInt4Oid);

    EXPECT_TRUE(RaisesError([&] { make_scalar_array_op(pstate, "@", true, left, right, 0); }));

    free_parsestate(pstate);
}

// ===========================================================================
// Function resolution tests (parse_func.cpp — catalog-driven)
//
// These tests verify the catalog-driven function lookup algorithm:
//   1. Exact match on (name, arg_count, arg_types) via pg_proc
//   2. Binary-coercible candidate selection
//   3. Argument coercion (make_fn_arguments)
//   4. Aggregate vs regular function distinction
//   5. Return type resolution per PostgreSQL semantics
// ===========================================================================

// --- IsAggregateFunction tests ---

TEST_F(ParseAnalysisTest, IsAggregateFunctionRecognizesAggregates) {
    EXPECT_TRUE(IsAggregateFunction("count"));
    EXPECT_TRUE(IsAggregateFunction("sum"));
    EXPECT_TRUE(IsAggregateFunction("min"));
    EXPECT_TRUE(IsAggregateFunction("max"));
    EXPECT_TRUE(IsAggregateFunction("avg"));
    // Case-insensitive.
    EXPECT_TRUE(IsAggregateFunction("COUNT"));
    EXPECT_TRUE(IsAggregateFunction("Sum"));
}

TEST_F(ParseAnalysisTest, IsAggregateFunctionRejectsRegularFunctions) {
    EXPECT_FALSE(IsAggregateFunction("lower"));
    EXPECT_FALSE(IsAggregateFunction("upper"));
    EXPECT_FALSE(IsAggregateFunction("length"));
    EXPECT_FALSE(IsAggregateFunction("date_trunc"));
    EXPECT_FALSE(IsAggregateFunction("nonexistent"));
}

// --- LookupFuncName tests ---

TEST_F(ParseAnalysisTest, LookupFuncNameFindsExactMatch) {
    std::vector<std::string> name = {"lower"};
    Oid argtypes[] = {kTextOid};
    FuncLookupResult result;
    ASSERT_TRUE(LookupFuncName(name, 1, argtypes, &result));
    EXPECT_EQ(result.rettype, kTextOid);
    EXPECT_FALSE(result.is_aggregate);
    EXPECT_FALSE(result.retset);
}

TEST_F(ParseAnalysisTest, LookupFuncNameFindsAggregate) {
    std::vector<std::string> name = {"sum"};
    Oid argtypes[] = {kInt4Oid};
    FuncLookupResult result;
    ASSERT_TRUE(LookupFuncName(name, 1, argtypes, &result));
    EXPECT_EQ(result.rettype, kInt8Oid);
    EXPECT_TRUE(result.is_aggregate);
}

TEST_F(ParseAnalysisTest, LookupFuncNameFindsByBinaryCoercibility) {
    // varchar is binary-coercible to text; lower(varchar) should resolve
    // to lower(text).
    std::vector<std::string> name = {"lower"};
    Oid argtypes[] = {kVarcharOid};
    FuncLookupResult result;
    ASSERT_TRUE(LookupFuncName(name, 1, argtypes, &result));
    EXPECT_EQ(result.rettype, kTextOid);
}

TEST_F(ParseAnalysisTest, LookupFuncNameReturnsFalseForNonexistent) {
    std::vector<std::string> name = {"nonexistent_func"};
    Oid argtypes[] = {kInt4Oid};
    FuncLookupResult result;
    EXPECT_FALSE(LookupFuncName(name, 1, argtypes, &result));
}

TEST_F(ParseAnalysisTest, LookupFuncNameReturnsFalseForWrongArgCount) {
    // lower takes 1 arg; calling with 2 should fail.
    std::vector<std::string> name = {"lower"};
    Oid argtypes[] = {kTextOid, kTextOid};
    FuncLookupResult result;
    EXPECT_FALSE(LookupFuncName(name, 2, argtypes, &result));
}

TEST_F(ParseAnalysisTest, LookupFuncNameResolvesOverloadedByArgCount) {
    // substring has 2-arg and 3-arg variants.
    std::vector<std::string> name2 = {"substring"};
    Oid argtypes2[] = {kTextOid, kInt4Oid};
    FuncLookupResult result2;
    ASSERT_TRUE(LookupFuncName(name2, 2, argtypes2, &result2));
    EXPECT_EQ(result2.rettype, kTextOid);

    std::vector<std::string> name3 = {"substring"};
    Oid argtypes3[] = {kTextOid, kInt4Oid, kInt4Oid};
    FuncLookupResult result3;
    ASSERT_TRUE(LookupFuncName(name3, 3, argtypes3, &result3));
    EXPECT_EQ(result3.rettype, kTextOid);
}

// --- transformFuncCall: regular function tests ---

TEST_F(ParseAnalysisTest, TransformFuncCallLowerText) {
    Query* qry = AnalyzeSingle("SELECT LOWER(event_type) FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kFuncExpr);
    EXPECT_EQ(exprType(tle->expr), kTextOid);
}

TEST_F(ParseAnalysisTest, TransformFuncCallUpperText) {
    Query* qry = AnalyzeSingle("SELECT UPPER(event_type) FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kFuncExpr);
    EXPECT_EQ(exprType(tle->expr), kTextOid);
}

TEST_F(ParseAnalysisTest, TransformFuncCallLengthText) {
    Query* qry = AnalyzeSingle("SELECT LENGTH(url) FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kFuncExpr);
    EXPECT_EQ(exprType(tle->expr), kInt4Oid);
}

TEST_F(ParseAnalysisTest, TransformFuncCallDateTrunc) {
    Query* qry = AnalyzeSingle("SELECT DATE_TRUNC('hour', event_time) FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kFuncExpr);
    EXPECT_EQ(exprType(tle->expr), kTimestampOid);
}

TEST_F(ParseAnalysisTest, TransformFuncCallAbsInt4) {
    Query* qry = AnalyzeSingle("SELECT ABS(count) FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kFuncExpr);
    EXPECT_EQ(exprType(tle->expr), kInt4Oid);
}

TEST_F(ParseAnalysisTest, TransformFuncCallUnknownLiteralResolvesToText) {
    // lower('hello') — 'hello' is unknown, should resolve to lower(text).
    Query* qry = AnalyzeSingle("SELECT LOWER('hello')");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kFuncExpr);
    EXPECT_EQ(exprType(tle->expr), kTextOid);
}

TEST_F(ParseAnalysisTest, TransformFuncCallNonexistentErrors) {
    // Parse separately from analyze so that stmts (a std::vector) is NOT on
    // the stack when ereport(ERROR) fires inside parse_analyze. longjmp
    // bypasses C++ destructors, so any std::vector local between PG_TRY and
    // the ereport would leak its internal buffer. By keeping stmts outside
    // PG_TRY, it is destructed normally when the test function returns.
    auto stmts = raw_parser("SELECT nonexistent_func(event_type) FROM hits");
    ASSERT_FALSE(stmts.empty());

    bool error = false;
    PG_TRY() {
        // parse_analyze takes const ref, so no copy of stmts is made.
        // The return value is being constructed when ereport fires — it is
        // never fully constructed, so nothing leaks.
        auto queries = parse_analyze(stmts, "SELECT nonexistent_func(event_type) FROM hits");
        (void)queries;
    }
    PG_CATCH() {
        error = true;
    }
    PG_END_TRY();
    EXPECT_TRUE(error);
}

// --- transformFuncCall: aggregate function tests ---

TEST_F(ParseAnalysisTest, TransformFuncCallCountStar) {
    Query* qry = AnalyzeSingle("SELECT COUNT(*) FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kAggref);
    auto* agg = static_cast<Aggref*>(tle->expr);
    EXPECT_TRUE(agg->aggstar);
    EXPECT_EQ(exprType(agg), kInt8Oid);
    EXPECT_TRUE(qry->has_aggs);
}

TEST_F(ParseAnalysisTest, TransformFuncCallCountInt8) {
    // COUNT(id) where id is int8 → count(int8) → int8.
    Query* qry = AnalyzeSingle("SELECT COUNT(id) FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kAggref);
    auto* agg = static_cast<Aggref*>(tle->expr);
    EXPECT_FALSE(agg->aggstar);
    EXPECT_EQ(exprType(agg), kInt8Oid);
}

TEST_F(ParseAnalysisTest, TransformFuncCallCountText) {
    // COUNT(event_type) where event_type is text → count(text) → int8.
    Query* qry = AnalyzeSingle("SELECT COUNT(event_type) FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kAggref);
    EXPECT_EQ(exprType(tle->expr), kInt8Oid);
}

TEST_F(ParseAnalysisTest, TransformFuncCallSumInt4) {
    // SUM(count) where count is int4 → sum(int4) → int8.
    Query* qry = AnalyzeSingle("SELECT SUM(count) FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kAggref);
    EXPECT_EQ(exprType(tle->expr), kInt8Oid);
}

TEST_F(ParseAnalysisTest, TransformFuncCallSumFloat8) {
    // SUM(price) where price is float8 → sum(float8) → float8.
    Query* qry = AnalyzeSingle("SELECT SUM(price) FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kAggref);
    EXPECT_EQ(exprType(tle->expr), kFloat8Oid);
}

TEST_F(ParseAnalysisTest, TransformFuncCallMinFloat8) {
    // MIN(price) where price is float8 → min(float8) → float8.
    Query* qry = AnalyzeSingle("SELECT MIN(price) FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kAggref);
    EXPECT_EQ(exprType(tle->expr), kFloat8Oid);
}

TEST_F(ParseAnalysisTest, TransformFuncCallMinDate) {
    // MIN(event_date) where event_date is date → min(date) → date.
    Query* qry = AnalyzeSingle("SELECT MIN(event_date) FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kAggref);
    EXPECT_EQ(exprType(tle->expr), kDateOid);
}

TEST_F(ParseAnalysisTest, TransformFuncCallMinText) {
    // MIN(url) where url is text → min(text) → text.
    Query* qry = AnalyzeSingle("SELECT MIN(url) FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kAggref);
    EXPECT_EQ(exprType(tle->expr), kTextOid);
}

TEST_F(ParseAnalysisTest, TransformFuncCallMaxFloat8) {
    Query* qry = AnalyzeSingle("SELECT MAX(price) FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kAggref);
    EXPECT_EQ(exprType(tle->expr), kFloat8Oid);
}

TEST_F(ParseAnalysisTest, TransformFuncCallMaxDate) {
    Query* qry = AnalyzeSingle("SELECT MAX(event_date) FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kAggref);
    EXPECT_EQ(exprType(tle->expr), kDateOid);
}

TEST_F(ParseAnalysisTest, TransformFuncCallAvgFloat8) {
    // AVG(price) where price is float8 → avg(float8) → float8.
    Query* qry = AnalyzeSingle("SELECT AVG(price) FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kAggref);
    EXPECT_EQ(exprType(tle->expr), kFloat8Oid);
}

TEST_F(ParseAnalysisTest, TransformFuncCallAvgInt4) {
    // AVG(count) where count is int4 → avg(int4) → float8.
    // MyToyDB computes AVG as float8 (numeric type not implemented).
    Query* qry = AnalyzeSingle("SELECT AVG(count) FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kAggref);
    EXPECT_EQ(exprType(tle->expr), kFloat8Oid);
}

TEST_F(ParseAnalysisTest, TransformFuncCallCountDistinct) {
    // COUNT(DISTINCT user_id) — should still resolve to count(int4) → int8.
    Query* qry = AnalyzeSingle("SELECT COUNT(DISTINCT user_id) FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kAggref);
    EXPECT_EQ(exprType(tle->expr), kInt8Oid);
}

TEST_F(ParseAnalysisTest, TransformFuncCallAggregateWithFilter) {
    // COUNT(*) FILTER (WHERE count > 0) — should set aggfilter.
    Query* qry = AnalyzeSingle("SELECT COUNT(*) FILTER (WHERE count > 0) FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kAggref);
    auto* agg = static_cast<Aggref*>(tle->expr);
    EXPECT_NE(agg->aggfilter, nullptr);
}

TEST_F(ParseAnalysisTest, TransformFuncCallAggregateWithOrderBy) {
    // SUM(count ORDER BY count) — should set aggorder.
    Query* qry = AnalyzeSingle("SELECT SUM(count ORDER BY count) FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kAggref);
    auto* agg = static_cast<Aggref*>(tle->expr);
    EXPECT_FALSE(agg->aggorder.empty());
}

// --- Function argument coercion tests ---

TEST_F(ParseAnalysisTest, TransformFuncCallCoercesVarcharToText) {
    // lower(varchar_column) — varchar is binary-coercible to text.
    // The argument should be coerced (wrapped in RelabelType).
    // We test this via LookupFuncName since the test table has no varchar
    // column; the binary-coercibility is verified at the lookup level.
    std::vector<std::string> name = {"lower"};
    Oid argtypes[] = {kVarcharOid};
    FuncLookupResult result;
    ASSERT_TRUE(LookupFuncName(name, 1, argtypes, &result));
    EXPECT_EQ(result.rettype, kTextOid);
}

TEST_F(ParseAnalysisTest, TransformFuncCallResolvesOverloadedSubstring) {
    // substr(text, int) — 2-arg variant (overloaded with 3-arg substr).
    // Uses regular function-call syntax; the SQL-standard SUBSTRING(str FROM
    // start) syntax is handled by the grammar's substr_list rule, which is
    // expanded in Phase O.
    Query* qry = AnalyzeSingle("SELECT substr(url, 1) FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    // substr should resolve to a function call (FuncExpr).
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kFuncExpr);
}

TEST_F(ParseAnalysisTest, TransformFuncCallRegexpReplaceThreeArgs) {
    Query* qry = AnalyzeSingle("SELECT REGEXP_REPLACE(url, 'a', 'b') FROM hits");
    ASSERT_NE(qry, nullptr);
    ASSERT_FALSE(qry->target_list.empty());
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    EXPECT_EQ(nodeTag(tle->expr), NodeTag::kFuncExpr);
    EXPECT_EQ(exprType(tle->expr), kTextOid);
}

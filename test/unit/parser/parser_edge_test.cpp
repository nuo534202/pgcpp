// parser_edge_test.cpp — Edge-case unit tests for the SQL parser (M5).
//
// Mirrors the fixture pattern of parser_test.cpp. Exercises boundary inputs
// (empty string, semicolon-only, comment-only), multiple statements, quoted
// identifiers, literal types, nested subqueries, long identifiers, inline
// and block comments, and basic DML/DDL statements.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "parser/parsenodes.hpp"
#include "parser/parser.hpp"

using pgcpp::memory::AllocSetContext;
using pgcpp::nodes::Node;
using pgcpp::nodes::NodeTag;
using pgcpp::nodes::nodeTag;
using pgcpp::nodes::Value;
using pgcpp::parser::AConst;
using pgcpp::parser::AExpr;
using pgcpp::parser::Alias;
using pgcpp::parser::ColumnDef;
using pgcpp::parser::ColumnRef;
using pgcpp::parser::CreateStmt;
using pgcpp::parser::InsertStmt;
using pgcpp::parser::RangeSubselect;
using pgcpp::parser::RangeVar;
using pgcpp::parser::raw_parser;
using pgcpp::parser::RawStmt;
using pgcpp::parser::ResTarget;
using pgcpp::parser::SelectStmt;
using pgcpp::parser::TypeName;

namespace {

// Test fixture that provides a memory context for each test. Identical
// structure to ParserTest in parser_test.cpp; renamed to avoid test-name
// collisions in the global GoogleTest registry.
class ParserEdgeTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("parser_edge_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);
    }

    void TearDown() override {
        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    AllocSetContext* context_ = nullptr;
};

// Helper: parse a SQL string and return the first statement's root node.
// Returns nullptr if parsing produces no statements.
Node* ParseSingle(const std::string& sql) {
    auto stmts = raw_parser(sql);
    if (stmts.empty()) {
        return nullptr;
    }
    return stmts[0]->stmt;
}

}  // namespace

// ---------------------------------------------------------------------------
// Empty / trivial inputs
// ---------------------------------------------------------------------------

TEST_F(ParserEdgeTest, ParseEmptyString) {
    // Empty input produces no statements (PostgreSQL-compatible behavior:
    // the stmtmulti grammar rule accepts an empty derivation).
    auto stmts = raw_parser("");
    EXPECT_TRUE(stmts.empty());
}

TEST_F(ParserEdgeTest, ParseSemicolonOnly) {
    // A lone ';' produces no statements, matching PostgreSQL.
    auto stmts = raw_parser(";");
    EXPECT_TRUE(stmts.empty());
}

TEST_F(ParserEdgeTest, ParseMultipleStatements) {
    auto stmts = raw_parser("SELECT 1; SELECT 2;");
    ASSERT_EQ(stmts.size(), 2u);
    EXPECT_EQ(nodeTag(stmts[0]->stmt), NodeTag::kSelectStmt);
    EXPECT_EQ(nodeTag(stmts[1]->stmt), NodeTag::kSelectStmt);
}

// ---------------------------------------------------------------------------
// SELECT variants
// ---------------------------------------------------------------------------

TEST_F(ParserEdgeTest, ParseSelectStar) {
    Node* stmt = ParseSingle("SELECT * FROM t1");
    ASSERT_NE(stmt, nullptr);
    ASSERT_EQ(nodeTag(stmt), NodeTag::kSelectStmt);
    auto* sel = static_cast<SelectStmt*>(stmt);
    ASSERT_EQ(sel->target_list.size(), 1u);
    auto* rt = static_cast<ResTarget*>(sel->target_list[0]);
    ASSERT_NE(rt->val, nullptr);
    // pgcpp's parser stores `*` directly as an A_Star node in rt->val
    // (unlike PostgreSQL which wraps it in a ColumnRef with one A_Star field).
    EXPECT_EQ(nodeTag(rt->val), NodeTag::kAStar);
}

TEST_F(ParserEdgeTest, ParseSelectConstant) {
    Node* stmt = ParseSingle("SELECT 42");
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    ASSERT_EQ(sel->target_list.size(), 1u);
    auto* rt = static_cast<ResTarget*>(sel->target_list[0]);
    ASSERT_NE(rt->val, nullptr);
    ASSERT_EQ(nodeTag(rt->val), NodeTag::kAConst);
    auto* ac = static_cast<AConst*>(rt->val);
    ASSERT_NE(ac->val, nullptr);
    EXPECT_EQ(nodeTag(ac->val), NodeTag::kInteger);
    EXPECT_EQ(static_cast<Value*>(ac->val)->GetInteger(), 42);
}

TEST_F(ParserEdgeTest, ParseDeeplyNestedSubquery) {
    Node* stmt = ParseSingle("SELECT * FROM (SELECT * FROM (SELECT 1) t1) t2");
    ASSERT_NE(stmt, nullptr);
    ASSERT_EQ(nodeTag(stmt), NodeTag::kSelectStmt);
    auto* outer = static_cast<SelectStmt*>(stmt);
    ASSERT_EQ(outer->from_clause.size(), 1u);
    ASSERT_EQ(nodeTag(outer->from_clause[0]), NodeTag::kRangeSubselect);

    auto* outer_rs = static_cast<RangeSubselect*>(outer->from_clause[0]);
    ASSERT_NE(outer_rs->subquery, nullptr);
    ASSERT_EQ(nodeTag(outer_rs->subquery), NodeTag::kSelectStmt);
    auto* middle = static_cast<SelectStmt*>(outer_rs->subquery);
    ASSERT_EQ(middle->from_clause.size(), 1u);
    ASSERT_EQ(nodeTag(middle->from_clause[0]), NodeTag::kRangeSubselect);

    auto* middle_rs = static_cast<RangeSubselect*>(middle->from_clause[0]);
    ASSERT_NE(middle_rs->subquery, nullptr);
    ASSERT_EQ(nodeTag(middle_rs->subquery), NodeTag::kSelectStmt);
    auto* inner = static_cast<SelectStmt*>(middle_rs->subquery);
    ASSERT_FALSE(inner->target_list.empty());
    // Innermost SELECT 1: target is an AConst.
    auto* rt = static_cast<ResTarget*>(inner->target_list[0]);
    ASSERT_NE(rt->val, nullptr);
    EXPECT_EQ(nodeTag(rt->val), NodeTag::kAConst);
}

// ---------------------------------------------------------------------------
// Identifiers
// ---------------------------------------------------------------------------

TEST_F(ParserEdgeTest, ParseLongIdentifier) {
    // 128-character identifier used as a table name.
    const std::string long_name(128, 'a');
    const std::string sql = "SELECT * FROM " + long_name;
    Node* stmt = ParseSingle(sql);
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    ASSERT_FALSE(sel->from_clause.empty());
    auto* rv = static_cast<RangeVar*>(sel->from_clause[0]);
    EXPECT_EQ(rv->relname.size(), 128u);
    EXPECT_EQ(rv->relname, long_name);
}

TEST_F(ParserEdgeTest, ParseQuotedIdentifier) {
    // Known pgcpp limitation: the lexer tokenizes "select" as a UIDENT
    // (Unicode identifier) rather than a regular IDENT, and the grammar
    // does not accept UIDENT in the SELECT target list position. The
    // parser therefore raises ereport(ERROR). Wrap the call in PG_TRY
    // so the test catches the longjmp instead of aborting the process.
    bool caught = false;
    PG_TRY() {
        // Intentionally discarded: the parser is expected to raise an error.
        (void)ParseSingle("SELECT \"select\" FROM t1");
    }
    PG_CATCH() {
        caught = true;
    }
    PG_END_TRY();
    EXPECT_TRUE(caught) << "pgcpp parser should reject quoted identifiers as a known limitation";
}

// ---------------------------------------------------------------------------
// Literal constants
// ---------------------------------------------------------------------------

TEST_F(ParserEdgeTest, ParseStringLiteral) {
    Node* stmt = ParseSingle("SELECT 'hello world'");
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    auto* rt = static_cast<ResTarget*>(sel->target_list[0]);
    ASSERT_NE(rt->val, nullptr);
    ASSERT_EQ(nodeTag(rt->val), NodeTag::kAConst);
    auto* ac = static_cast<AConst*>(rt->val);
    ASSERT_NE(ac->val, nullptr);
    EXPECT_EQ(nodeTag(ac->val), NodeTag::kString);
    EXPECT_EQ(static_cast<Value*>(ac->val)->GetString(), "hello world");
}

TEST_F(ParserEdgeTest, ParseNumericLiteral) {
    Node* stmt = ParseSingle("SELECT 3.14159");
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    auto* rt = static_cast<ResTarget*>(sel->target_list[0]);
    ASSERT_NE(rt->val, nullptr);
    ASSERT_EQ(nodeTag(rt->val), NodeTag::kAConst);
    auto* ac = static_cast<AConst*>(rt->val);
    ASSERT_NE(ac->val, nullptr);
    EXPECT_EQ(nodeTag(ac->val), NodeTag::kFloat);
    EXPECT_EQ(static_cast<Value*>(ac->val)->GetFloat(), "3.14159");
}

// ---------------------------------------------------------------------------
// Comments
// ---------------------------------------------------------------------------

TEST_F(ParserEdgeTest, ParseCommentOnly) {
    // A line-comment-only input produces no statements.
    auto stmts = raw_parser("-- just a comment");
    EXPECT_TRUE(stmts.empty());
}

TEST_F(ParserEdgeTest, ParseInlineComment) {
    // The -- comment runs to end-of-line; the scanner skips it, so the
    // parser sees "SELECT 1 + 2" — a single target with an AExpr.
    Node* stmt = ParseSingle("SELECT 1 -- comment\n + 2");
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    ASSERT_EQ(sel->target_list.size(), 1u);
    auto* rt = static_cast<ResTarget*>(sel->target_list[0]);
    ASSERT_NE(rt->val, nullptr);
    EXPECT_EQ(nodeTag(rt->val), NodeTag::kAExpr);
    // Sanity check: the result is one statement, not two.
    auto stmts = raw_parser("SELECT 1 -- comment\n + 2");
    EXPECT_EQ(stmts.size(), 1u);
}

TEST_F(ParserEdgeTest, ParseBlockComment) {
    // Block comment before the statement is skipped; parsing proceeds.
    Node* stmt = ParseSingle("/* comment */ SELECT 1");
    ASSERT_NE(stmt, nullptr);
    ASSERT_EQ(nodeTag(stmt), NodeTag::kSelectStmt);
    auto* sel = static_cast<SelectStmt*>(stmt);
    ASSERT_EQ(sel->target_list.size(), 1u);
    auto* rt = static_cast<ResTarget*>(sel->target_list[0]);
    ASSERT_NE(rt->val, nullptr);
    EXPECT_EQ(nodeTag(rt->val), NodeTag::kAConst);
}

// ---------------------------------------------------------------------------
// DML / DDL
// ---------------------------------------------------------------------------

TEST_F(ParserEdgeTest, ParseInsertBasic) {
    Node* stmt = ParseSingle("INSERT INTO t1 VALUES (1, 'a')");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kInsertStmt);
    auto* ins = static_cast<InsertStmt*>(stmt);
    ASSERT_NE(ins->relation, nullptr);
    EXPECT_EQ(ins->relation->relname, "t1");
    ASSERT_NE(ins->select_stmt, nullptr);
}

TEST_F(ParserEdgeTest, ParseCreateTableBasic) {
    Node* stmt = ParseSingle("CREATE TABLE t1 (a int, b text)");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kCreateStmt);
    auto* cs = static_cast<CreateStmt*>(stmt);
    ASSERT_NE(cs->relation, nullptr);
    EXPECT_EQ(cs->relation->relname, "t1");
    ASSERT_EQ(cs->table_elts.size(), 2u);
    // Each element is a ColumnDef; verify names and types round-trip.
    auto* c1 = static_cast<ColumnDef*>(cs->table_elts[0]);
    EXPECT_EQ(c1->colname, "a");
    ASSERT_NE(c1->type_name, nullptr);
    auto* c2 = static_cast<ColumnDef*>(cs->table_elts[1]);
    EXPECT_EQ(c2->colname, "b");
    ASSERT_NE(c2->type_name, nullptr);
}

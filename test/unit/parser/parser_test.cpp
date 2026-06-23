// parser_test.cpp — Unit tests for the SQL parser (M5).
//
// Tests the raw_parser() function which converts SQL strings into
// RawStmt parse trees. Verifies that ClickBench-relevant SQL constructs
// parse correctly and produce the expected AST node types.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "mytoydb/parser/parser.h"
#include "mytoydb/parser/parsenodes.h"
#include "mytoydb/common/containers/node.h"
#include "mytoydb/common/memory/alloc_set.h"
#include "mytoydb/common/memory/memory_context.h"
#include "mytoydb/common/error/elog.h"

using mytoydb::parser::raw_parser;
using mytoydb::parser::RawStmt;
using mytoydb::parser::SelectStmt;
using mytoydb::parser::InsertStmt;
using mytoydb::parser::UpdateStmt;
using mytoydb::parser::DeleteStmt;
using mytoydb::parser::CreateStmt;
using mytoydb::parser::DropStmt;
using mytoydb::parser::AlterTableStmt;
using mytoydb::parser::AlterTableCmd;
using mytoydb::parser::TransactionStmt;
using mytoydb::parser::TruncateStmt;
using mytoydb::parser::ExplainStmt;
using mytoydb::parser::IndexStmt;
using mytoydb::parser::ViewStmt;
using mytoydb::parser::CreateAsStmt;
using mytoydb::parser::VacuumStmt;
using mytoydb::parser::VariableSetStmt;
using mytoydb::parser::ClusterStmt;
using mytoydb::parser::LockStmt;
using mytoydb::parser::DiscardStmt;
using mytoydb::parser::NotifyStmt;
using mytoydb::parser::ListenStmt;
using mytoydb::parser::UnlistenStmt;
using mytoydb::parser::CheckPointStmt;
using mytoydb::parser::ReindexStmt;
using mytoydb::parser::DeallocateStmt;
using mytoydb::parser::PrepareStmt;
using mytoydb::parser::ExecuteStmt;
using mytoydb::parser::LoadStmt;
using mytoydb::parser::CallStmt;
using mytoydb::parser::RenameStmt;
using mytoydb::parser::CreateSeqStmt;
using mytoydb::parser::AlterSeqStmt;
using mytoydb::parser::CreateFunctionStmt;
using mytoydb::parser::CreateTrigStmt;
using mytoydb::parser::CreateRoleStmt;
using mytoydb::parser::DropRoleStmt;
using mytoydb::parser::GrantStmt;
using mytoydb::parser::CopyStmt;
using mytoydb::parser::RefreshMatViewStmt;
using mytoydb::parser::CreateTableSpaceStmt;
using mytoydb::parser::DropTableSpaceStmt;
using mytoydb::parser::CreatedbStmt;
using mytoydb::parser::DropdbStmt;
using mytoydb::parser::AlterDatabaseStmt;
using mytoydb::parser::DropBehavior;
using mytoydb::parser::AlterTableType;
using mytoydb::parser::ColumnRef;
using mytoydb::parser::AConst;
using mytoydb::parser::FuncCall;
using mytoydb::parser::AExpr;
using mytoydb::parser::ResTarget;
using mytoydb::parser::RangeVar;
using mytoydb::parser::Alias;
using mytoydb::parser::TypeName;
using mytoydb::parser::ColumnDef;
using mytoydb::nodes::Node;
using mytoydb::nodes::NodeTag;
using mytoydb::nodes::nodeTag;
using mytoydb::memory::AllocSetContext;

namespace {

// Test fixture that provides a memory context for each test.
class ParserTest : public ::testing::Test {
protected:
    void SetUp() override {
        mytoydb::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("parser_test_context");
        mytoydb::memory::SetCurrentMemoryContext(context_);
    }

    void TearDown() override {
        mytoydb::memory::SetCurrentMemoryContext(nullptr);
    }

    AllocSetContext* context_ = nullptr;
};

// Helper: parse a SQL string and return the first statement's root node.
// Returns nullptr if parsing produces no statements.
Node* ParseSingle(const std::string& sql) {
    auto stmts = raw_parser(sql);
    if (stmts.empty()) return nullptr;
    return stmts[0]->stmt;
}

}  // namespace

// ---------------------------------------------------------------------------
// Basic SELECT
// ---------------------------------------------------------------------------

TEST_F(ParserTest, SelectConstant) {
    Node* stmt = ParseSingle("SELECT 1");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kSelectStmt);
    auto* sel = static_cast<SelectStmt*>(stmt);
    ASSERT_FALSE(sel->target_list.empty());
    // target_list[0] should be a ResTarget wrapping an AConst
    Node* target = sel->target_list[0];
    ASSERT_EQ(nodeTag(target), NodeTag::kResTarget);
    auto* rt = static_cast<ResTarget*>(target);
    ASSERT_NE(rt->val, nullptr);
    EXPECT_EQ(nodeTag(rt->val), NodeTag::kAConst);
}

TEST_F(ParserTest, SelectStar) {
    Node* stmt = ParseSingle("SELECT *");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kSelectStmt);
    auto* sel = static_cast<SelectStmt*>(stmt);
    ASSERT_FALSE(sel->target_list.empty());
}

TEST_F(ParserTest, SelectColumnRef) {
    Node* stmt = ParseSingle("SELECT col1");
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    ASSERT_FALSE(sel->target_list.empty());
    auto* rt = static_cast<ResTarget*>(sel->target_list[0]);
    ASSERT_NE(rt->val, nullptr);
    EXPECT_EQ(nodeTag(rt->val), NodeTag::kColumnRef);
}

TEST_F(ParserTest, SelectFromTable) {
    Node* stmt = ParseSingle("SELECT * FROM hits");
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    ASSERT_FALSE(sel->from_clause.empty());
    Node* from_item = sel->from_clause[0];
    EXPECT_EQ(nodeTag(from_item), NodeTag::kRangeVar);
    auto* rv = static_cast<RangeVar*>(from_item);
    EXPECT_EQ(rv->relname, "hits");
}

TEST_F(ParserTest, SelectMultipleColumns) {
    Node* stmt = ParseSingle("SELECT a, b, c FROM t");
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    EXPECT_EQ(sel->target_list.size(), 3u);
}

TEST_F(ParserTest, SelectWithAlias) {
    Node* stmt = ParseSingle("SELECT COUNT(*) AS cnt FROM t");
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    ASSERT_EQ(sel->target_list.size(), 1u);
    auto* rt = static_cast<ResTarget*>(sel->target_list[0]);
    EXPECT_EQ(rt->name, "cnt");
    EXPECT_EQ(nodeTag(rt->val), NodeTag::kFuncCall);
}

// ---------------------------------------------------------------------------
// WHERE clause
// ---------------------------------------------------------------------------

TEST_F(ParserTest, SelectWhereClause) {
    Node* stmt = ParseSingle("SELECT * FROM t WHERE a = 1");
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    ASSERT_NE(sel->where_clause, nullptr);
}

TEST_F(ParserTest, SelectWhereAndClause) {
    Node* stmt = ParseSingle("SELECT * FROM t WHERE a = 1 AND b > 2");
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    ASSERT_NE(sel->where_clause, nullptr);
    EXPECT_EQ(nodeTag(sel->where_clause), NodeTag::kAExpr);
}

TEST_F(ParserTest, SelectWhereLike) {
    Node* stmt = ParseSingle("SELECT * FROM hits WHERE URL LIKE '%google%'");
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    ASSERT_NE(sel->where_clause, nullptr);
}

TEST_F(ParserTest, SelectWhereNotLike) {
    Node* stmt = ParseSingle("SELECT * FROM hits WHERE URL NOT LIKE '%.google.%'");
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    ASSERT_NE(sel->where_clause, nullptr);
}

TEST_F(ParserTest, SelectWhereIn) {
    Node* stmt = ParseSingle("SELECT * FROM t WHERE a IN (1, 2, 3)");
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    ASSERT_NE(sel->where_clause, nullptr);
}

// ---------------------------------------------------------------------------
// GROUP BY / HAVING / ORDER BY / LIMIT / OFFSET
// ---------------------------------------------------------------------------

TEST_F(ParserTest, SelectGroupBy) {
    Node* stmt = ParseSingle("SELECT a, COUNT(*) FROM t GROUP BY a");
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    EXPECT_FALSE(sel->group_clause.empty());
}

TEST_F(ParserTest, SelectGroupByMultiple) {
    Node* stmt = ParseSingle("SELECT a, b, COUNT(*) FROM t GROUP BY a, b");
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    EXPECT_EQ(sel->group_clause.size(), 2u);
}

TEST_F(ParserTest, SelectHaving) {
    Node* stmt = ParseSingle(
        "SELECT a, COUNT(*) FROM t GROUP BY a HAVING COUNT(*) > 100");
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    ASSERT_NE(sel->having_clause, nullptr);
}

TEST_F(ParserTest, SelectOrderBy) {
    Node* stmt = ParseSingle("SELECT * FROM t ORDER BY a DESC");
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    EXPECT_FALSE(sel->sort_clause.empty());
}

TEST_F(ParserTest, SelectOrderByMultiple) {
    Node* stmt = ParseSingle("SELECT * FROM t ORDER BY a, b DESC LIMIT 10");
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    EXPECT_EQ(sel->sort_clause.size(), 2u);
    EXPECT_NE(sel->limit_count, nullptr);
}

TEST_F(ParserTest, SelectLimitOffset) {
    Node* stmt = ParseSingle("SELECT * FROM t LIMIT 10 OFFSET 5");
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    EXPECT_NE(sel->limit_count, nullptr);
    EXPECT_NE(sel->limit_offset, nullptr);
}

// ---------------------------------------------------------------------------
// Aggregate functions
// ---------------------------------------------------------------------------

TEST_F(ParserTest, SelectCountStar) {
    Node* stmt = ParseSingle("SELECT COUNT(*) FROM t");
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    auto* rt = static_cast<ResTarget*>(sel->target_list[0]);
    EXPECT_EQ(nodeTag(rt->val), NodeTag::kFuncCall);
}

TEST_F(ParserTest, SelectCountDistinct) {
    Node* stmt = ParseSingle("SELECT COUNT(DISTINCT UserID) FROM t");
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    auto* rt = static_cast<ResTarget*>(sel->target_list[0]);
    ASSERT_EQ(nodeTag(rt->val), NodeTag::kFuncCall);
    auto* fc = static_cast<FuncCall*>(rt->val);
    EXPECT_TRUE(fc->agg_distinct);
}

TEST_F(ParserTest, SelectSumAvg) {
    Node* stmt = ParseSingle("SELECT SUM(a), AVG(b) FROM t");
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    EXPECT_EQ(sel->target_list.size(), 2u);
}

TEST_F(ParserTest, SelectMinMax) {
    Node* stmt = ParseSingle("SELECT MIN(a), MAX(b) FROM t");
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    EXPECT_EQ(sel->target_list.size(), 2u);
}

// ---------------------------------------------------------------------------
// Functions: length, extract, date_trunc, regexp_replace
// ---------------------------------------------------------------------------

TEST_F(ParserTest, SelectLengthFunc) {
    Node* stmt = ParseSingle("SELECT length(URL) FROM hits");
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    auto* rt = static_cast<ResTarget*>(sel->target_list[0]);
    EXPECT_EQ(nodeTag(rt->val), NodeTag::kFuncCall);
}

TEST_F(ParserTest, SelectExtractFunc) {
    Node* stmt = ParseSingle(
        "SELECT extract(minute FROM EventTime) FROM hits");
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    auto* rt = static_cast<ResTarget*>(sel->target_list[0]);
    EXPECT_EQ(nodeTag(rt->val), NodeTag::kFuncCall);
}

TEST_F(ParserTest, SelectDateTruncFunc) {
    Node* stmt = ParseSingle(
        "SELECT DATE_TRUNC('minute', EventTime) FROM hits");
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    auto* rt = static_cast<ResTarget*>(sel->target_list[0]);
    EXPECT_EQ(nodeTag(rt->val), NodeTag::kFuncCall);
}

TEST_F(ParserTest, SelectRegexpReplaceFunc) {
    Node* stmt = ParseSingle(
        "SELECT REGEXP_REPLACE(Referer, '^https?://(?:www\\.)?([^/]+)/.*$', '\\1') FROM hits");
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    auto* rt = static_cast<ResTarget*>(sel->target_list[0]);
    EXPECT_EQ(nodeTag(rt->val), NodeTag::kFuncCall);
}

// ---------------------------------------------------------------------------
// CASE expression
// ---------------------------------------------------------------------------

TEST_F(ParserTest, SelectCaseExpr) {
    Node* stmt = ParseSingle(
        "SELECT CASE WHEN a = 0 THEN 'x' ELSE 'y' END FROM t");
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    auto* rt = static_cast<ResTarget*>(sel->target_list[0]);
    ASSERT_NE(rt->val, nullptr);
}

// ---------------------------------------------------------------------------
// Arithmetic expressions
// ---------------------------------------------------------------------------

TEST_F(ParserTest, SelectArithmeticExpr) {
    Node* stmt = ParseSingle("SELECT a + 1, b - 2, c * 3, d / 4 FROM t");
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    EXPECT_EQ(sel->target_list.size(), 4u);
}

TEST_F(ParserTest, SelectComplexArithmetic) {
    Node* stmt = ParseSingle(
        "SELECT SUM(ResolutionWidth + 1) FROM hits");
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    EXPECT_EQ(sel->target_list.size(), 1u);
}

// ---------------------------------------------------------------------------
// CREATE TABLE
// ---------------------------------------------------------------------------

TEST_F(ParserTest, CreateTableBasic) {
    Node* stmt = ParseSingle("CREATE TABLE t (a INTEGER, b TEXT)");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kCreateStmt);
    auto* cs = static_cast<CreateStmt*>(stmt);
    ASSERT_NE(cs->relation, nullptr);
    EXPECT_EQ(cs->relation->relname, "t");
    EXPECT_EQ(cs->table_elts.size(), 2u);
}

TEST_F(ParserTest, CreateTableClickBench) {
    Node* stmt = ParseSingle(
        "CREATE TABLE hits ("
        "  WatchID BIGINT NOT NULL, "
        "  Title TEXT NOT NULL, "
        "  EventTime TIMESTAMP NOT NULL, "
        "  EventDate Date NOT NULL, "
        "  CounterID INTEGER NOT NULL"
        ")");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kCreateStmt);
    auto* cs = static_cast<CreateStmt*>(stmt);
    EXPECT_EQ(cs->table_elts.size(), 5u);
}

// ---------------------------------------------------------------------------
// Multiple statements
// ---------------------------------------------------------------------------

TEST_F(ParserTest, MultipleStatements) {
    auto stmts = raw_parser("SELECT 1; SELECT 2;");
    EXPECT_EQ(stmts.size(), 2u);
}

TEST_F(ParserTest, SingleStatementWithSemicolon) {
    auto stmts = raw_parser("SELECT 1;");
    EXPECT_EQ(stmts.size(), 1u);
}

// ---------------------------------------------------------------------------
// ClickBench query patterns
// ---------------------------------------------------------------------------

TEST_F(ParserTest, ClickBenchQ1) {
    Node* stmt = ParseSingle("SELECT COUNT(*) FROM hits");
    ASSERT_NE(stmt, nullptr);
}

TEST_F(ParserTest, ClickBenchQ2) {
    Node* stmt = ParseSingle(
        "SELECT COUNT(*) FROM hits WHERE AdvEngineID <> 0");
    ASSERT_NE(stmt, nullptr);
}

TEST_F(ParserTest, ClickBenchQ3) {
    Node* stmt = ParseSingle(
        "SELECT SUM(AdvEngineID), COUNT(*), AVG(ResolutionWidth) FROM hits");
    ASSERT_NE(stmt, nullptr);
}

TEST_F(ParserTest, ClickBenchQ5) {
    Node* stmt = ParseSingle(
        "SELECT COUNT(DISTINCT UserID) FROM hits");
    ASSERT_NE(stmt, nullptr);
}

TEST_F(ParserTest, ClickBenchQ7) {
    Node* stmt = ParseSingle(
        "SELECT MIN(EventDate), MAX(EventDate) FROM hits");
    ASSERT_NE(stmt, nullptr);
}

TEST_F(ParserTest, ClickBenchQ8) {
    Node* stmt = ParseSingle(
        "SELECT AdvEngineID, COUNT(*) FROM hits "
        "WHERE AdvEngineID <> 0 "
        "GROUP BY AdvEngineID "
        "ORDER BY COUNT(*) DESC");
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    EXPECT_FALSE(sel->group_clause.empty());
    EXPECT_FALSE(sel->sort_clause.empty());
}

TEST_F(ParserTest, ClickBenchQ9) {
    Node* stmt = ParseSingle(
        "SELECT RegionID, COUNT(DISTINCT UserID) AS u "
        "FROM hits GROUP BY RegionID ORDER BY u DESC LIMIT 10");
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    EXPECT_FALSE(sel->group_clause.empty());
    EXPECT_FALSE(sel->sort_clause.empty());
    EXPECT_NE(sel->limit_count, nullptr);
}

TEST_F(ParserTest, ClickBenchQ19) {
    Node* stmt = ParseSingle(
        "SELECT UserID, extract(minute FROM EventTime) AS m, "
        "SearchPhrase, COUNT(*) FROM hits "
        "GROUP BY UserID, m, SearchPhrase "
        "ORDER BY COUNT(*) DESC LIMIT 10");
    ASSERT_NE(stmt, nullptr);
}

TEST_F(ParserTest, ClickBenchQ21) {
    Node* stmt = ParseSingle(
        "SELECT COUNT(*) FROM hits WHERE URL LIKE '%google%'");
    ASSERT_NE(stmt, nullptr);
}

TEST_F(ParserTest, ClickBenchQ22) {
    Node* stmt = ParseSingle(
        "SELECT SearchPhrase, MIN(URL), COUNT(*) AS c "
        "FROM hits WHERE URL LIKE '%google%' AND SearchPhrase <> '' "
        "GROUP BY SearchPhrase ORDER BY c DESC LIMIT 10");
    ASSERT_NE(stmt, nullptr);
}

TEST_F(ParserTest, ClickBenchQ23) {
    Node* stmt = ParseSingle(
        "SELECT SearchPhrase, MIN(URL), MIN(Title), COUNT(*) AS c, "
        "COUNT(DISTINCT UserID) FROM hits "
        "WHERE Title LIKE '%Google%' AND URL NOT LIKE '%.google.%' "
        "AND SearchPhrase <> '' "
        "GROUP BY SearchPhrase ORDER BY c DESC LIMIT 10");
    ASSERT_NE(stmt, nullptr);
}

TEST_F(ParserTest, ClickBenchQ28) {
    Node* stmt = ParseSingle(
        "SELECT CounterID, AVG(length(URL)) AS l, COUNT(*) AS c "
        "FROM hits WHERE URL <> '' "
        "GROUP BY CounterID HAVING COUNT(*) > 100000 "
        "ORDER BY l DESC LIMIT 25");
    ASSERT_NE(stmt, nullptr);
}

TEST_F(ParserTest, ClickBenchQ29) {
    Node* stmt = ParseSingle(
        "SELECT REGEXP_REPLACE(Referer, '^https?://(?:www\\.)?([^/]+)/.*$', '\\1') AS k, "
        "AVG(length(Referer)) AS l, COUNT(*) AS c, MIN(Referer) "
        "FROM hits WHERE Referer <> '' "
        "GROUP BY k HAVING COUNT(*) > 100000 "
        "ORDER BY l DESC LIMIT 25");
    ASSERT_NE(stmt, nullptr);
}

TEST_F(ParserTest, ClickBenchQ37) {
    Node* stmt = ParseSingle(
        "SELECT URL, COUNT(*) AS PageViews FROM hits "
        "WHERE CounterID = 62 AND EventDate >= '2013-07-01' "
        "AND EventDate <= '2013-07-31' AND DontCountHits = 0 "
        "AND IsRefresh = 0 AND URL <> '' "
        "GROUP BY URL ORDER BY PageViews DESC LIMIT 10");
    ASSERT_NE(stmt, nullptr);
}

TEST_F(ParserTest, ClickBenchQ40) {
    Node* stmt = ParseSingle(
        "SELECT TraficSourceID, SearchEngineID, AdvEngineID, "
        "CASE WHEN (SearchEngineID = 0 AND AdvEngineID = 0) THEN Referer ELSE '' END AS Src, "
        "URL AS Dst, COUNT(*) AS PageViews FROM hits "
        "WHERE CounterID = 62 AND EventDate >= '2013-07-01' "
        "AND EventDate <= '2013-07-31' AND IsRefresh = 0 "
        "GROUP BY TraficSourceID, SearchEngineID, AdvEngineID, Src, Dst "
        "ORDER BY PageViews DESC LIMIT 10 OFFSET 1000");
    ASSERT_NE(stmt, nullptr);
}

TEST_F(ParserTest, ClickBenchQ43) {
    Node* stmt = ParseSingle(
        "SELECT DATE_TRUNC('minute', EventTime) AS M, COUNT(*) AS PageViews "
        "FROM hits WHERE CounterID = 62 AND EventDate >= '2013-07-14' "
        "AND EventDate <= '2013-07-15' AND IsRefresh = 0 "
        "AND DontCountHits = 0 "
        "GROUP BY DATE_TRUNC('minute', EventTime) "
        "ORDER BY DATE_TRUNC('minute', EventTime) LIMIT 10 OFFSET 1000");
    ASSERT_NE(stmt, nullptr);
}

// ===========================================================================
// Phase 5 grammar expansion: DML statements
// ===========================================================================

// ---------------------------------------------------------------------------
// INSERT
// ---------------------------------------------------------------------------

TEST_F(ParserTest, InsertBasic) {
    Node* stmt = ParseSingle("INSERT INTO t VALUES (1, 'a')");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kInsertStmt);
    auto* ins = static_cast<InsertStmt*>(stmt);
    ASSERT_NE(ins->relation, nullptr);
    EXPECT_EQ(ins->relation->relname, "t");
}

TEST_F(ParserTest, InsertWithColumns) {
    Node* stmt = ParseSingle("INSERT INTO t (a, b) VALUES (1, 'a')");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kInsertStmt);
    auto* ins = static_cast<InsertStmt*>(stmt);
    EXPECT_EQ(ins->cols.size(), 2u);
}

TEST_F(ParserTest, InsertFromSelect) {
    Node* stmt = ParseSingle("INSERT INTO t SELECT * FROM s");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kInsertStmt);
    auto* ins = static_cast<InsertStmt*>(stmt);
    ASSERT_NE(ins->select_stmt, nullptr);
}

// ---------------------------------------------------------------------------
// UPDATE
// ---------------------------------------------------------------------------

TEST_F(ParserTest, UpdateBasic) {
    Node* stmt = ParseSingle("UPDATE t SET a = 1 WHERE b = 2");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kUpdateStmt);
    auto* upd = static_cast<UpdateStmt*>(stmt);
    ASSERT_NE(upd->relation, nullptr);
    EXPECT_EQ(upd->relation->relname, "t");
    EXPECT_FALSE(upd->target_list.empty());
    ASSERT_NE(upd->where_clause, nullptr);
}

TEST_F(ParserTest, UpdateMultipleColumns) {
    Node* stmt = ParseSingle("UPDATE t SET a = 1, b = 2, c = 3");
    ASSERT_NE(stmt, nullptr);
    auto* upd = static_cast<UpdateStmt*>(stmt);
    EXPECT_EQ(upd->target_list.size(), 3u);
}

// ---------------------------------------------------------------------------
// DELETE
// ---------------------------------------------------------------------------

TEST_F(ParserTest, DeleteBasic) {
    Node* stmt = ParseSingle("DELETE FROM t WHERE a = 1");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kDeleteStmt);
    auto* del = static_cast<DeleteStmt*>(stmt);
    ASSERT_NE(del->relation, nullptr);
    EXPECT_EQ(del->relation->relname, "t");
    ASSERT_NE(del->where_clause, nullptr);
}

TEST_F(ParserTest, DeleteAll) {
    Node* stmt = ParseSingle("DELETE FROM t");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kDeleteStmt);
    auto* del = static_cast<DeleteStmt*>(stmt);
    EXPECT_EQ(del->where_clause, nullptr);
}

// ===========================================================================
// Phase 5 grammar expansion: DDL statements
// ===========================================================================

// ---------------------------------------------------------------------------
// DROP TABLE
// ---------------------------------------------------------------------------

TEST_F(ParserTest, DropTableBasic) {
    Node* stmt = ParseSingle("DROP TABLE t");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kDropStmt);
    auto* drop = static_cast<DropStmt*>(stmt);
    EXPECT_FALSE(drop->objects.empty());
}

TEST_F(ParserTest, DropTableIfExists) {
    Node* stmt = ParseSingle("DROP TABLE IF EXISTS t");
    ASSERT_NE(stmt, nullptr);
    auto* drop = static_cast<DropStmt*>(stmt);
    EXPECT_TRUE(drop->missing_ok);
}

TEST_F(ParserTest, DropTableCascade) {
    Node* stmt = ParseSingle("DROP TABLE t CASCADE");
    ASSERT_NE(stmt, nullptr);
    auto* drop = static_cast<DropStmt*>(stmt);
    EXPECT_EQ(drop->behavior, DropBehavior::kCascade);
}

// ---------------------------------------------------------------------------
// ALTER TABLE
// ---------------------------------------------------------------------------

TEST_F(ParserTest, AlterTableAddColumn) {
    Node* stmt = ParseSingle("ALTER TABLE t ADD COLUMN c INTEGER");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kAlterTableStmt);
    auto* at = static_cast<AlterTableStmt*>(stmt);
    ASSERT_NE(at->relation, nullptr);
    EXPECT_EQ(at->relation->relname, "t");
    EXPECT_EQ(at->cmds.size(), 1u);
}

TEST_F(ParserTest, AlterTableDropColumn) {
    Node* stmt = ParseSingle("ALTER TABLE t DROP COLUMN c");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kAlterTableStmt);
    auto* at = static_cast<AlterTableStmt*>(stmt);
    EXPECT_EQ(at->cmds.size(), 1u);
    auto* cmd = static_cast<AlterTableCmd*>(at->cmds[0]);
    EXPECT_EQ(cmd->subtype, AlterTableType::kDropColumn);
}

TEST_F(ParserTest, AlterTableRenameColumn) {
    Node* stmt = ParseSingle("ALTER TABLE t RENAME COLUMN a TO b");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kRenameStmt);
    auto* rn = static_cast<RenameStmt*>(stmt);
    EXPECT_EQ(rn->subname, "a");
    EXPECT_EQ(rn->newname, "b");
}

TEST_F(ParserTest, AlterTableRenameTo) {
    Node* stmt = ParseSingle("ALTER TABLE t RENAME TO t2");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kRenameStmt);
    auto* rn = static_cast<RenameStmt*>(stmt);
    EXPECT_EQ(rn->newname, "t2");
}

// ---------------------------------------------------------------------------
// CREATE INDEX
// ---------------------------------------------------------------------------

TEST_F(ParserTest, CreateIndexBasic) {
    Node* stmt = ParseSingle("CREATE INDEX idx ON t (a)");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kIndexStmt);
    auto* idx = static_cast<IndexStmt*>(stmt);
    EXPECT_EQ(idx->idxname, "idx");
    ASSERT_NE(idx->relation, nullptr);
    EXPECT_EQ(idx->relation->relname, "t");
    EXPECT_FALSE(idx->index_params.empty());
}

TEST_F(ParserTest, CreateUniqueIndex) {
    Node* stmt = ParseSingle("CREATE UNIQUE INDEX idx ON t (a)");
    ASSERT_NE(stmt, nullptr);
    auto* idx = static_cast<IndexStmt*>(stmt);
    EXPECT_TRUE(idx->unique);
}

TEST_F(ParserTest, CreateIndexConcurrently) {
    Node* stmt = ParseSingle("CREATE INDEX CONCURRENTLY idx ON t (a)");
    ASSERT_NE(stmt, nullptr);
    auto* idx = static_cast<IndexStmt*>(stmt);
    EXPECT_TRUE(idx->concurrent);
}

TEST_F(ParserTest, CreateIndexIfNotExists) {
    Node* stmt = ParseSingle("CREATE INDEX IF NOT EXISTS idx ON t (a)");
    ASSERT_NE(stmt, nullptr);
    auto* idx = static_cast<IndexStmt*>(stmt);
    EXPECT_TRUE(idx->if_not_exists);
}

// ---------------------------------------------------------------------------
// CREATE VIEW
// ---------------------------------------------------------------------------

TEST_F(ParserTest, CreateViewBasic) {
    Node* stmt = ParseSingle("CREATE VIEW v AS SELECT * FROM t");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kViewStmt);
    auto* v = static_cast<ViewStmt*>(stmt);
    ASSERT_NE(v->view, nullptr);
    EXPECT_EQ(v->view->relname, "v");
    ASSERT_NE(v->query, nullptr);
}

TEST_F(ParserTest, CreateOrReplaceView) {
    Node* stmt = ParseSingle("CREATE OR REPLACE VIEW v AS SELECT * FROM t");
    ASSERT_NE(stmt, nullptr);
    auto* v = static_cast<ViewStmt*>(stmt);
    EXPECT_TRUE(v->replace);
}

// ---------------------------------------------------------------------------
// CREATE TABLE AS
// ---------------------------------------------------------------------------

TEST_F(ParserTest, CreateTableAsSelect) {
    Node* stmt = ParseSingle("CREATE TABLE t AS SELECT * FROM s");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kCreateAsStmt);
    auto* ca = static_cast<CreateAsStmt*>(stmt);
    ASSERT_NE(ca->query, nullptr);
}

// ---------------------------------------------------------------------------
// CREATE SEQUENCE
// ---------------------------------------------------------------------------

TEST_F(ParserTest, CreateSequenceBasic) {
    Node* stmt = ParseSingle("CREATE SEQUENCE seq");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kCreateSeqStmt);
    auto* cs = static_cast<CreateSeqStmt*>(stmt);
    ASSERT_NE(cs->sequence, nullptr);
    EXPECT_EQ(cs->sequence->relname, "seq");
}

TEST_F(ParserTest, CreateSequenceIfNotExists) {
    Node* stmt = ParseSingle("CREATE SEQUENCE IF NOT EXISTS seq");
    ASSERT_NE(stmt, nullptr);
    auto* cs = static_cast<CreateSeqStmt*>(stmt);
    EXPECT_TRUE(cs->if_not_exists);
}

// ---------------------------------------------------------------------------
// ALTER SEQUENCE
// ---------------------------------------------------------------------------

TEST_F(ParserTest, AlterSequenceBasic) {
    Node* stmt = ParseSingle("ALTER SEQUENCE seq RESTART");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kAlterSeqStmt);
    auto* as = static_cast<AlterSeqStmt*>(stmt);
    ASSERT_NE(as->sequence, nullptr);
    EXPECT_EQ(as->sequence->relname, "seq");
}

// ===========================================================================
// Phase 5 grammar expansion: Transaction control
// ===========================================================================

TEST_F(ParserTest, TransactionBegin) {
    Node* stmt = ParseSingle("BEGIN");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kTransactionStmt);
    auto* tx = static_cast<TransactionStmt*>(stmt);
    EXPECT_EQ(tx->kind, TransactionStmt::Kind::kBegin);
}

TEST_F(ParserTest, TransactionCommit) {
    Node* stmt = ParseSingle("COMMIT");
    ASSERT_NE(stmt, nullptr);
    auto* tx = static_cast<TransactionStmt*>(stmt);
    EXPECT_EQ(tx->kind, TransactionStmt::Kind::kCommit);
}

TEST_F(ParserTest, TransactionRollback) {
    Node* stmt = ParseSingle("ROLLBACK");
    ASSERT_NE(stmt, nullptr);
    auto* tx = static_cast<TransactionStmt*>(stmt);
    EXPECT_EQ(tx->kind, TransactionStmt::Kind::kRollback);
}

TEST_F(ParserTest, TransactionStart) {
    Node* stmt = ParseSingle("START TRANSACTION");
    ASSERT_NE(stmt, nullptr);
    auto* tx = static_cast<TransactionStmt*>(stmt);
    EXPECT_EQ(tx->kind, TransactionStmt::Kind::kStart);
}

// ===========================================================================
// Phase 5 grammar expansion: Utility statements
// ===========================================================================

// ---------------------------------------------------------------------------
// TRUNCATE
// ---------------------------------------------------------------------------

TEST_F(ParserTest, TruncateBasic) {
    Node* stmt = ParseSingle("TRUNCATE TABLE t");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kTruncateStmt);
    auto* tr = static_cast<TruncateStmt*>(stmt);
    EXPECT_FALSE(tr->relations.empty());
}

TEST_F(ParserTest, TruncateMultipleTables) {
    Node* stmt = ParseSingle("TRUNCATE TABLE t1, t2, t3");
    ASSERT_NE(stmt, nullptr);
    auto* tr = static_cast<TruncateStmt*>(stmt);
    EXPECT_EQ(tr->relations.size(), 3u);
}

TEST_F(ParserTest, TruncateRestartIdentity) {
    Node* stmt = ParseSingle("TRUNCATE TABLE t RESTART IDENTITY");
    ASSERT_NE(stmt, nullptr);
    auto* tr = static_cast<TruncateStmt*>(stmt);
    EXPECT_TRUE(tr->restart_seqs);
}

TEST_F(ParserTest, TruncateCascade) {
    Node* stmt = ParseSingle("TRUNCATE TABLE t CASCADE");
    ASSERT_NE(stmt, nullptr);
    auto* tr = static_cast<TruncateStmt*>(stmt);
    EXPECT_EQ(tr->behavior, DropBehavior::kCascade);
}

// ---------------------------------------------------------------------------
// EXPLAIN
// ---------------------------------------------------------------------------

TEST_F(ParserTest, ExplainSelect) {
    Node* stmt = ParseSingle("EXPLAIN SELECT * FROM t");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kExplainStmt);
    auto* ex = static_cast<ExplainStmt*>(stmt);
    ASSERT_NE(ex->query, nullptr);
}

TEST_F(ParserTest, ExplainAnalyze) {
    Node* stmt = ParseSingle("EXPLAIN ANALYZE SELECT * FROM t");
    ASSERT_NE(stmt, nullptr);
    auto* ex = static_cast<ExplainStmt*>(stmt);
    EXPECT_FALSE(ex->options.empty());
}

// ---------------------------------------------------------------------------
// VACUUM / ANALYZE
// ---------------------------------------------------------------------------

TEST_F(ParserTest, VacuumBasic) {
    Node* stmt = ParseSingle("VACUUM");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kVacuumStmt);
    auto* v = static_cast<VacuumStmt*>(stmt);
    EXPECT_TRUE(v->is_vacuumcmd);
}

TEST_F(ParserTest, VacuumTable) {
    Node* stmt = ParseSingle("VACUUM t");
    ASSERT_NE(stmt, nullptr);
    auto* v = static_cast<VacuumStmt*>(stmt);
    EXPECT_FALSE(v->rels.empty());
}

TEST_F(ParserTest, AnalyzeBasic) {
    Node* stmt = ParseSingle("ANALYZE");
    ASSERT_NE(stmt, nullptr);
    auto* v = static_cast<VacuumStmt*>(stmt);
    EXPECT_FALSE(v->is_vacuumcmd);
}

TEST_F(ParserTest, AnalyzeTable) {
    Node* stmt = ParseSingle("ANALYZE t");
    ASSERT_NE(stmt, nullptr);
    auto* v = static_cast<VacuumStmt*>(stmt);
    EXPECT_FALSE(v->rels.empty());
}

// ---------------------------------------------------------------------------
// SET / RESET
// ---------------------------------------------------------------------------

TEST_F(ParserTest, SetBasic) {
    Node* stmt = ParseSingle("SET search_path TO public");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kVariableSetStmt);
    auto* vs = static_cast<VariableSetStmt*>(stmt);
    EXPECT_EQ(vs->name, "search_path");
}

TEST_F(ParserTest, SetEqual) {
    Node* stmt = ParseSingle("SET search_path = 'public'");
    ASSERT_NE(stmt, nullptr);
    auto* vs = static_cast<VariableSetStmt*>(stmt);
    EXPECT_EQ(vs->name, "search_path");
}

TEST_F(ParserTest, ResetBasic) {
    Node* stmt = ParseSingle("RESET search_path");
    ASSERT_NE(stmt, nullptr);
    auto* vs = static_cast<VariableSetStmt*>(stmt);
    EXPECT_EQ(vs->kind, VariableSetStmt::Kind::kReset);
}

TEST_F(ParserTest, ResetAll) {
    Node* stmt = ParseSingle("RESET ALL");
    ASSERT_NE(stmt, nullptr);
    auto* vs = static_cast<VariableSetStmt*>(stmt);
    EXPECT_EQ(vs->kind, VariableSetStmt::Kind::kResetAll);
}

// ---------------------------------------------------------------------------
// CLUSTER
// ---------------------------------------------------------------------------

TEST_F(ParserTest, ClusterBasic) {
    Node* stmt = ParseSingle("CLUSTER t");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kClusterStmt);
    auto* c = static_cast<ClusterStmt*>(stmt);
    ASSERT_NE(c->relation, nullptr);
    EXPECT_EQ(c->relation->relname, "t");
}

TEST_F(ParserTest, ClusterUsingIndex) {
    Node* stmt = ParseSingle("CLUSTER t USING idx");
    ASSERT_NE(stmt, nullptr);
    auto* c = static_cast<ClusterStmt*>(stmt);
    EXPECT_EQ(c->indexname, "idx");
}

// ---------------------------------------------------------------------------
// LOCK TABLE
// ---------------------------------------------------------------------------

TEST_F(ParserTest, LockTableBasic) {
    Node* stmt = ParseSingle("LOCK TABLE t");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kLockStmt);
    auto* l = static_cast<LockStmt*>(stmt);
    EXPECT_FALSE(l->relations.empty());
}

TEST_F(ParserTest, LockTableInMode) {
    Node* stmt = ParseSingle("LOCK TABLE t IN ACCESS EXCLUSIVE MODE");
    ASSERT_NE(stmt, nullptr);
    auto* l = static_cast<LockStmt*>(stmt);
    EXPECT_FALSE(l->relations.empty());
}

// ---------------------------------------------------------------------------
// DISCARD
// ---------------------------------------------------------------------------

TEST_F(ParserTest, DiscardAll) {
    Node* stmt = ParseSingle("DISCARD ALL");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kDiscardStmt);
    auto* d = static_cast<DiscardStmt*>(stmt);
    EXPECT_EQ(d->target, DiscardStmt::Target::kAll);
}

TEST_F(ParserTest, DiscardTemp) {
    Node* stmt = ParseSingle("DISCARD TEMP");
    ASSERT_NE(stmt, nullptr);
    auto* d = static_cast<DiscardStmt*>(stmt);
    EXPECT_EQ(d->target, DiscardStmt::Target::kTemp);
}

// ---------------------------------------------------------------------------
// LISTEN / NOTIFY / UNLISTEN
// ---------------------------------------------------------------------------

TEST_F(ParserTest, ListenBasic) {
    Node* stmt = ParseSingle("LISTEN channel1");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kListenStmt);
    auto* l = static_cast<ListenStmt*>(stmt);
    EXPECT_EQ(l->conditionname, "channel1");
}

TEST_F(ParserTest, NotifyBasic) {
    Node* stmt = ParseSingle("NOTIFY channel1");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kNotifyStmt);
    auto* n = static_cast<NotifyStmt*>(stmt);
    EXPECT_EQ(n->conditionname, "channel1");
}

TEST_F(ParserTest, NotifyWithPayload) {
    Node* stmt = ParseSingle("NOTIFY channel1, 'payload'");
    ASSERT_NE(stmt, nullptr);
    auto* n = static_cast<NotifyStmt*>(stmt);
    EXPECT_EQ(n->conditionname, "channel1");
    EXPECT_EQ(n->payload, "payload");
}

TEST_F(ParserTest, UnlistenBasic) {
    Node* stmt = ParseSingle("UNLISTEN channel1");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kUnlistenStmt);
    auto* u = static_cast<UnlistenStmt*>(stmt);
    EXPECT_EQ(u->conditionname, "channel1");
}

// ---------------------------------------------------------------------------
// CHECKPOINT
// ---------------------------------------------------------------------------

TEST_F(ParserTest, CheckpointBasic) {
    Node* stmt = ParseSingle("CHECKPOINT");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kCheckPointStmt);
}

// ---------------------------------------------------------------------------
// REINDEX
// ---------------------------------------------------------------------------

TEST_F(ParserTest, ReindexTable) {
    Node* stmt = ParseSingle("REINDEX TABLE t");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kReindexStmt);
    auto* ri = static_cast<ReindexStmt*>(stmt);
    EXPECT_EQ(ri->kind, ReindexStmt::Kind::kTable);
}

TEST_F(ParserTest, ReindexIndex) {
    Node* stmt = ParseSingle("REINDEX INDEX idx");
    ASSERT_NE(stmt, nullptr);
    auto* ri = static_cast<ReindexStmt*>(stmt);
    EXPECT_EQ(ri->kind, ReindexStmt::Kind::kIndex);
}

TEST_F(ParserTest, ReindexSchema) {
    Node* stmt = ParseSingle("REINDEX SCHEMA s");
    ASSERT_NE(stmt, nullptr);
    auto* ri = static_cast<ReindexStmt*>(stmt);
    EXPECT_EQ(ri->kind, ReindexStmt::Kind::kSchema);
}

// ---------------------------------------------------------------------------
// DEALLOCATE
// ---------------------------------------------------------------------------

TEST_F(ParserTest, DeallocateBasic) {
    Node* stmt = ParseSingle("DEALLOCATE stmt1");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kDeallocateStmt);
    auto* d = static_cast<DeallocateStmt*>(stmt);
    EXPECT_EQ(d->name, "stmt1");
}

TEST_F(ParserTest, DeallocateAll) {
    Node* stmt = ParseSingle("DEALLOCATE ALL");
    ASSERT_NE(stmt, nullptr);
    auto* d = static_cast<DeallocateStmt*>(stmt);
    EXPECT_TRUE(d->name.empty());
}

// ---------------------------------------------------------------------------
// PREPARE / EXECUTE
// ---------------------------------------------------------------------------

TEST_F(ParserTest, PrepareBasic) {
    Node* stmt = ParseSingle("PREPARE stmt1 AS SELECT * FROM t");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kPrepareStmt);
    auto* p = static_cast<PrepareStmt*>(stmt);
    EXPECT_EQ(p->name, "stmt1");
    ASSERT_NE(p->query, nullptr);
}

TEST_F(ParserTest, PrepareWithTypes) {
    Node* stmt = ParseSingle("PREPARE stmt1 (int, text) AS SELECT * FROM t");
    ASSERT_NE(stmt, nullptr);
    auto* p = static_cast<PrepareStmt*>(stmt);
    EXPECT_EQ(p->argtypes.size(), 2u);
}

TEST_F(ParserTest, ExecuteBasic) {
    Node* stmt = ParseSingle("EXECUTE stmt1");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kExecuteStmt);
    auto* e = static_cast<ExecuteStmt*>(stmt);
    EXPECT_EQ(e->name, "stmt1");
}

TEST_F(ParserTest, ExecuteWithParams) {
    Node* stmt = ParseSingle("EXECUTE stmt1 (1, 'a')");
    ASSERT_NE(stmt, nullptr);
    auto* e = static_cast<ExecuteStmt*>(stmt);
    EXPECT_EQ(e->name, "stmt1");
    EXPECT_FALSE(e->params.empty());
}

// ---------------------------------------------------------------------------
// LOAD
// ---------------------------------------------------------------------------

TEST_F(ParserTest, LoadBasic) {
    Node* stmt = ParseSingle("LOAD '/path/to/lib'");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kLoadStmt);
    auto* l = static_cast<LoadStmt*>(stmt);
    EXPECT_EQ(l->filename, "/path/to/lib");
}

// ---------------------------------------------------------------------------
// CALL (procedure call)
// ---------------------------------------------------------------------------

TEST_F(ParserTest, CallBasic) {
    Node* stmt = ParseSingle("CALL proc(1, 'a')");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kCallStmt);
    auto* c = static_cast<CallStmt*>(stmt);
    ASSERT_NE(c->funccall, nullptr);
}

// ===========================================================================
// Phase 5 grammar expansion: Role & privilege management
// ===========================================================================

TEST_F(ParserTest, CreateRoleBasic) {
    Node* stmt = ParseSingle("CREATE ROLE r1");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kCreateRoleStmt);
    auto* cr = static_cast<CreateRoleStmt*>(stmt);
    EXPECT_EQ(cr->role, "r1");
}

TEST_F(ParserTest, CreateUserBasic) {
    Node* stmt = ParseSingle("CREATE USER u1");
    ASSERT_NE(stmt, nullptr);
    auto* cr = static_cast<CreateRoleStmt*>(stmt);
    EXPECT_EQ(cr->stmt_type, CreateRoleStmt::Kind::kUser);
}

TEST_F(ParserTest, CreateRoleWithLogin) {
    Node* stmt = ParseSingle("CREATE ROLE r1 LOGIN");
    ASSERT_NE(stmt, nullptr);
    auto* cr = static_cast<CreateRoleStmt*>(stmt);
    EXPECT_FALSE(cr->options.empty());
}

TEST_F(ParserTest, DropRoleBasic) {
    Node* stmt = ParseSingle("DROP ROLE r1");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kDropRoleStmt);
    auto* dr = static_cast<DropRoleStmt*>(stmt);
    EXPECT_FALSE(dr->roles.empty());
}

TEST_F(ParserTest, DropRoleIfExists) {
    Node* stmt = ParseSingle("DROP ROLE IF EXISTS r1");
    ASSERT_NE(stmt, nullptr);
    auto* dr = static_cast<DropRoleStmt*>(stmt);
    EXPECT_TRUE(dr->missing_ok);
}

TEST_F(ParserTest, GrantBasic) {
    Node* stmt = ParseSingle("GRANT SELECT ON t TO r1");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kGrantStmt);
    auto* g = static_cast<GrantStmt*>(stmt);
    EXPECT_TRUE(g->is_grant);
    EXPECT_FALSE(g->grantees.empty());
}

TEST_F(ParserTest, RevokeBasic) {
    Node* stmt = ParseSingle("REVOKE SELECT ON t FROM r1");
    ASSERT_NE(stmt, nullptr);
    auto* g = static_cast<GrantStmt*>(stmt);
    EXPECT_FALSE(g->is_grant);
}

// ===========================================================================
// Phase 5 grammar expansion: Database & tablespace management
// ===========================================================================

TEST_F(ParserTest, CreateDatabaseBasic) {
    Node* stmt = ParseSingle("CREATE DATABASE db1");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kCreatedbStmt);
    auto* cd = static_cast<CreatedbStmt*>(stmt);
    EXPECT_EQ(cd->dbname, "db1");
}

TEST_F(ParserTest, DropDatabaseBasic) {
    Node* stmt = ParseSingle("DROP DATABASE db1");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kDropdbStmt);
    auto* dd = static_cast<DropdbStmt*>(stmt);
    EXPECT_EQ(dd->dbname, "db1");
}

TEST_F(ParserTest, DropDatabaseIfExists) {
    Node* stmt = ParseSingle("DROP DATABASE IF EXISTS db1");
    ASSERT_NE(stmt, nullptr);
    auto* dd = static_cast<DropdbStmt*>(stmt);
    EXPECT_TRUE(dd->missing_ok);
}

TEST_F(ParserTest, CreateTablespaceBasic) {
    Node* stmt = ParseSingle("CREATE TABLESPACE ts1 LOCATION '/data'");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kCreateTableSpaceStmt);
    auto* ct = static_cast<CreateTableSpaceStmt*>(stmt);
    EXPECT_EQ(ct->tablespacename, "ts1");
    EXPECT_EQ(ct->location, "/data");
}

TEST_F(ParserTest, DropTablespaceBasic) {
    Node* stmt = ParseSingle("DROP TABLESPACE ts1");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kDropTableSpaceStmt);
    auto* dt = static_cast<DropTableSpaceStmt*>(stmt);
    EXPECT_EQ(dt->tablespacename, "ts1");
}

// ===========================================================================
// Phase 5 grammar expansion: Other utility statements
// ===========================================================================

// ---------------------------------------------------------------------------
// COPY
// ---------------------------------------------------------------------------

TEST_F(ParserTest, CopyTo) {
    Node* stmt = ParseSingle("COPY t TO '/path/to/file'");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kCopyStmt);
    auto* c = static_cast<CopyStmt*>(stmt);
    ASSERT_NE(c->relation, nullptr);
    EXPECT_EQ(c->relation->relname, "t");
    EXPECT_FALSE(c->is_from);
    EXPECT_EQ(c->filename, "/path/to/file");
}

TEST_F(ParserTest, CopyFrom) {
    Node* stmt = ParseSingle("COPY t FROM '/path/to/file'");
    ASSERT_NE(stmt, nullptr);
    auto* c = static_cast<CopyStmt*>(stmt);
    EXPECT_TRUE(c->is_from);
}

// ---------------------------------------------------------------------------
// REFRESH MATERIALIZED VIEW
// ---------------------------------------------------------------------------

TEST_F(ParserTest, RefreshMatViewBasic) {
    Node* stmt = ParseSingle("REFRESH MATERIALIZED VIEW mv");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kRefreshMatViewStmt);
    auto* r = static_cast<RefreshMatViewStmt*>(stmt);
    ASSERT_NE(r->relation, nullptr);
    EXPECT_EQ(r->relation->relname, "mv");
}

// ---------------------------------------------------------------------------
// CREATE FUNCTION (basic)
// ---------------------------------------------------------------------------

TEST_F(ParserTest, CreateFunctionBasic) {
    Node* stmt = ParseSingle(
        "CREATE FUNCTION fn(a int) RETURNS int AS $$ SELECT 1 $$ LANGUAGE sql");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kCreateFunctionStmt);
    auto* cf = static_cast<CreateFunctionStmt*>(stmt);
    EXPECT_FALSE(cf->funcname.empty());
}

// ---------------------------------------------------------------------------
// CREATE TRIGGER (basic)
// ---------------------------------------------------------------------------

TEST_F(ParserTest, CreateTriggerBasic) {
    Node* stmt = ParseSingle(
        "CREATE TRIGGER trg BEFORE INSERT ON t FOR EACH ROW EXECUTE FUNCTION fn()");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(nodeTag(stmt), NodeTag::kCreateTrigStmt);
    auto* ct = static_cast<CreateTrigStmt*>(stmt);
    EXPECT_EQ(ct->trigname, "trg");
    ASSERT_NE(ct->relation, nullptr);
    EXPECT_EQ(ct->relation->relname, "t");
}

// parse_utilcmd_test.cpp — Unit tests for parse_utilcmd (M5 Task 15.11.1).
//
// Tests transformCreateStmt / transformAlterTableStmt / transformIndexStmt,
// the DDL parse-analysis entry points converted from PostgreSQL 15's
// src/backend/parser/parse_utilcmd.c.
//
// The transformed statements still wrap as CMD_UTILITY Query nodes, but
// column types are validated, column defaults and CHECK constraints are
// cooked, and duplicate column/constraint names are rejected at parse time.

#include "pgcpp/parser/parse_utilcmd.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "pgcpp/catalog/bootstrap_catalog.hpp"
#include "pgcpp/catalog/catalog.hpp"
#include "pgcpp/catalog/pg_attribute.hpp"
#include "pgcpp/catalog/pg_class.hpp"
#include "pgcpp/catalog/syscache.hpp"
#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/alloc_set.hpp"
#include "pgcpp/common/memory/memory_context.hpp"
#include "pgcpp/parser/analyze.hpp"
#include "pgcpp/parser/parse_node.hpp"
#include "pgcpp/parser/parse_type.hpp"
#include "pgcpp/parser/parsenodes.hpp"
#include "pgcpp/parser/parser.hpp"
#include "pgcpp/parser/primnodes.hpp"
#include "pgcpp/types/datum.hpp"

using mytoydb::catalog::BootstrapCatalog;
using mytoydb::catalog::Catalog;
using mytoydb::catalog::GetCatalog;
using mytoydb::catalog::GetSysCache;
using mytoydb::catalog::kInvalidOid;
using mytoydb::catalog::Oid;
using mytoydb::catalog::SetCatalog;
using mytoydb::catalog::SetSysCache;
using mytoydb::catalog::SysCache;
using mytoydb::memory::AllocSetContext;
using mytoydb::nodes::makePallocNode;
using mytoydb::nodes::Node;
using mytoydb::nodes::NodeTag;
using mytoydb::nodes::nodeTag;
using mytoydb::parser::AlterTableCmd;
using mytoydb::parser::AlterTableStmt;
using mytoydb::parser::AlterTableType;
using mytoydb::parser::CmdType;
using mytoydb::parser::ColumnDef;
using mytoydb::parser::CreateStmt;
using mytoydb::parser::DefElem;
using mytoydb::parser::DropBehavior;
using mytoydb::parser::IndexElem;
using mytoydb::parser::IndexStmt;
using mytoydb::parser::make_parsestate;
using mytoydb::parser::ParseState;
using mytoydb::parser::Query;
using mytoydb::parser::RangeVar;
using mytoydb::parser::raw_parser;
using mytoydb::parser::RawStmt;
using mytoydb::parser::SortByDir;
using mytoydb::parser::transformAlterTableStmt;
using mytoydb::parser::transformCreateStmt;
using mytoydb::parser::transformIndexStmt;
using mytoydb::parser::TypeName;
using mytoydb::parser::typenameTypeId;
using mytoydb::types::kInt4Oid;
using mytoydb::types::kInt8Oid;
using mytoydb::types::kTextOid;
using mytoydb::types::kTimestampOid;

namespace {

// Test fixture: memory context + catalog + syscache.
class ParseUtilCmdTest : public ::testing::Test {
protected:
    void SetUp() override {
        mytoydb::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("parse_utilcmd_test_context");
        mytoydb::memory::SetCurrentMemoryContext(context_);

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

        mytoydb::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    // Parse a single statement (no analysis). Used to build raw stmts to
    // feed to the transform functions under test.
    Node* ParseSingle(const char* sql) {
        auto stmts = raw_parser(sql);
        if (stmts.empty())
            return nullptr;
        return stmts[0]->stmt;
    }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
    SysCache* syscache_ = nullptr;
};

// Helper: run fn inside PG_TRY and report whether it ereported.
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

// Find the DefElem with the given defname in a list of nodes; else nullptr.
DefElem* FindDefElem(const std::vector<Node*>& nodes, const std::string& name) {
    for (Node* n : nodes) {
        if (n == nullptr || n->GetTag() != NodeTag::kDefElem)
            continue;
        auto* d = static_cast<DefElem*>(n);
        if (d->defname == name)
            return d;
    }
    return nullptr;
}

}  // namespace

// ===========================================================================
// transformCreateStmt
// ===========================================================================

TEST_F(ParseUtilCmdTest, TransformCreateStmtReturnsUtilityQuery) {
    Node* stmt = ParseSingle("CREATE TABLE t (a INTEGER, b TEXT)");
    ASSERT_NE(stmt, nullptr);
    ASSERT_EQ(nodeTag(stmt), NodeTag::kCreateStmt);

    ParseState* pstate = make_parsestate(nullptr);
    Query* qry = transformCreateStmt(pstate, static_cast<CreateStmt*>(stmt));
    ASSERT_NE(qry, nullptr);
    EXPECT_EQ(qry->command_type, CmdType::kUtility);
    EXPECT_NE(qry->utility_stmt, nullptr);
    EXPECT_EQ(qry->utility_stmt, stmt);
    free_parsestate(pstate);
}

TEST_F(ParseUtilCmdTest, TransformCreateStmtResolvesColumnTypes) {
    Node* stmt = ParseSingle("CREATE TABLE t (a INTEGER, b TEXT, c BIGINT)");
    ASSERT_NE(stmt, nullptr);
    auto* cs = static_cast<CreateStmt*>(stmt);
    ASSERT_EQ(cs->table_elts.size(), 3u);

    ParseState* pstate = make_parsestate(nullptr);
    transformCreateStmt(pstate, cs);
    free_parsestate(pstate);

    // After transform, each ColumnDef's type_name should have its type_oid set.
    auto* c0 = static_cast<ColumnDef*>(cs->table_elts[0]);
    ASSERT_NE(c0->type_name, nullptr);
    EXPECT_EQ(c0->type_name->type_oid, kInt4Oid);

    auto* c1 = static_cast<ColumnDef*>(cs->table_elts[1]);
    ASSERT_NE(c1->type_name, nullptr);
    EXPECT_EQ(c1->type_name->type_oid, kTextOid);

    auto* c2 = static_cast<ColumnDef*>(cs->table_elts[2]);
    ASSERT_NE(c2->type_name, nullptr);
    EXPECT_EQ(c2->type_name->type_oid, kInt8Oid);
}

TEST_F(ParseUtilCmdTest, TransformCreateStmtRejectsUnknownType) {
    Node* stmt = ParseSingle("CREATE TABLE t (a nonexistent_type)");
    ASSERT_NE(stmt, nullptr);
    auto* cs = static_cast<CreateStmt*>(stmt);

    ParseState* pstate = make_parsestate(nullptr);
    EXPECT_TRUE(RaisesError([&] { transformCreateStmt(pstate, cs); }));
    free_parsestate(pstate);
}

TEST_F(ParseUtilCmdTest, TransformCreateStmtRejectsDuplicateColumnNames) {
    Node* stmt = ParseSingle("CREATE TABLE t (a INTEGER, a TEXT)");
    ASSERT_NE(stmt, nullptr);
    auto* cs = static_cast<CreateStmt*>(stmt);

    ParseState* pstate = make_parsestate(nullptr);
    EXPECT_TRUE(RaisesError([&] { transformCreateStmt(pstate, cs); }));
    free_parsestate(pstate);
}

TEST_F(ParseUtilCmdTest, TransformCreateStmtCooksColumnDefault) {
    Node* stmt = ParseSingle("CREATE TABLE t (a INTEGER DEFAULT 42)");
    ASSERT_NE(stmt, nullptr);
    auto* cs = static_cast<CreateStmt*>(stmt);
    ASSERT_EQ(cs->table_elts.size(), 1u);
    auto* coldef = static_cast<ColumnDef*>(cs->table_elts[0]);

    // Before transform, cooked_default should be nullptr.
    EXPECT_EQ(coldef->cooked_default, nullptr);

    ParseState* pstate = make_parsestate(nullptr);
    transformCreateStmt(pstate, cs);
    free_parsestate(pstate);

    // After transform, cooked_default should be set (to a Const node).
    ASSERT_NE(coldef->cooked_default, nullptr);
    EXPECT_EQ(nodeTag(coldef->cooked_default), NodeTag::kConst);
}

TEST_F(ParseUtilCmdTest, TransformCreateStmtCooksCheckConstraint) {
    Node* stmt = ParseSingle("CREATE TABLE t (a INTEGER CHECK (a > 0))");
    ASSERT_NE(stmt, nullptr);
    auto* cs = static_cast<CreateStmt*>(stmt);
    ASSERT_EQ(cs->table_elts.size(), 1u);
    auto* coldef = static_cast<ColumnDef*>(cs->table_elts[0]);

    ParseState* pstate = make_parsestate(nullptr);
    transformCreateStmt(pstate, cs);
    free_parsestate(pstate);

    // The CHECK constraint should be transformed from a raw a_expr (AExpr)
    // to a transformed expression (OpExpr).
    DefElem* check = FindDefElem(coldef->constraints, "check");
    ASSERT_NE(check, nullptr);
    ASSERT_NE(check->arg, nullptr);
    // After transform, the arg should be an OpExpr (">" applied to var and const).
    EXPECT_EQ(nodeTag(check->arg), NodeTag::kOpExpr);
}

TEST_F(ParseUtilCmdTest, TransformCreateStmtMarksNotNull) {
    Node* stmt = ParseSingle("CREATE TABLE t (a INTEGER NOT NULL)");
    ASSERT_NE(stmt, nullptr);
    auto* cs = static_cast<CreateStmt*>(stmt);
    ASSERT_EQ(cs->table_elts.size(), 1u);
    auto* coldef = static_cast<ColumnDef*>(cs->table_elts[0]);

    ParseState* pstate = make_parsestate(nullptr);
    transformCreateStmt(pstate, cs);
    free_parsestate(pstate);

    EXPECT_TRUE(coldef->is_not_null);
}

TEST_F(ParseUtilCmdTest, TransformCreateStmtClickBenchSchema) {
    // A schema approximating the ClickBench "hits" table.
    Node* stmt = ParseSingle(
        "CREATE TABLE hits ("
        "  WatchID BIGINT NOT NULL, "
        "  Title TEXT NOT NULL, "
        "  EventTime TIMESTAMP NOT NULL"
        ")");
    ASSERT_NE(stmt, nullptr);
    auto* cs = static_cast<CreateStmt*>(stmt);

    ParseState* pstate = make_parsestate(nullptr);
    transformCreateStmt(pstate, cs);
    free_parsestate(pstate);

    ASSERT_EQ(cs->table_elts.size(), 3u);
    auto* c0 = static_cast<ColumnDef*>(cs->table_elts[0]);
    // Note: this project's scanner does NOT downcase unquoted identifiers
    // (a deviation from PostgreSQL). The case is preserved as written.
    EXPECT_EQ(c0->colname, "WatchID");
    EXPECT_EQ(c0->type_name->type_oid, kInt8Oid);
    EXPECT_TRUE(c0->is_not_null);

    auto* c2 = static_cast<ColumnDef*>(cs->table_elts[2]);
    EXPECT_EQ(c2->type_name->type_oid, kTimestampOid);
    EXPECT_TRUE(c2->is_not_null);
}

// ===========================================================================
// transformAlterTableStmt
// ===========================================================================

TEST_F(ParseUtilCmdTest, TransformAlterTableStmtAddColumnResolvesType) {
    // ALTER TABLE t ADD COLUMN c INTEGER
    Node* stmt = ParseSingle("ALTER TABLE t ADD COLUMN c INTEGER");
    ASSERT_NE(stmt, nullptr);
    ASSERT_EQ(nodeTag(stmt), NodeTag::kAlterTableStmt);
    auto* ats = static_cast<AlterTableStmt*>(stmt);
    ASSERT_EQ(ats->cmds.size(), 1u);

    auto* cmd = static_cast<AlterTableCmd*>(ats->cmds[0]);
    ASSERT_EQ(cmd->subtype, AlterTableType::kAddColumn);
    ASSERT_NE(cmd->def, nullptr);
    ASSERT_EQ(nodeTag(cmd->def), NodeTag::kColumnDef);
    auto* coldef = static_cast<ColumnDef*>(cmd->def);

    ParseState* pstate = make_parsestate(nullptr);
    transformAlterTableStmt(pstate, ats);
    free_parsestate(pstate);

    EXPECT_EQ(coldef->type_name->type_oid, kInt4Oid);
}

TEST_F(ParseUtilCmdTest, TransformAlterTableStmtReturnsUtilityQuery) {
    Node* stmt = ParseSingle("ALTER TABLE t ADD COLUMN c INTEGER");
    ASSERT_NE(stmt, nullptr);
    auto* ats = static_cast<AlterTableStmt*>(stmt);

    ParseState* pstate = make_parsestate(nullptr);
    Query* qry = transformAlterTableStmt(pstate, ats);
    ASSERT_NE(qry, nullptr);
    EXPECT_EQ(qry->command_type, CmdType::kUtility);
    EXPECT_EQ(qry->utility_stmt, stmt);
    free_parsestate(pstate);
}

// ===========================================================================
// transformIndexStmt
// ===========================================================================

TEST_F(ParseUtilCmdTest, TransformIndexStmtReturnsUtilityQuery) {
    Node* stmt = ParseSingle("CREATE INDEX idx ON t (a)");
    ASSERT_NE(stmt, nullptr);
    ASSERT_EQ(nodeTag(stmt), NodeTag::kIndexStmt);
    auto* ist = static_cast<IndexStmt*>(stmt);

    ParseState* pstate = make_parsestate(nullptr);
    Query* qry = transformIndexStmt(pstate, ist);
    ASSERT_NE(qry, nullptr);
    EXPECT_EQ(qry->command_type, CmdType::kUtility);
    EXPECT_EQ(qry->utility_stmt, stmt);
    free_parsestate(pstate);
}

TEST_F(ParseUtilCmdTest, TransformIndexStmtKeepsIndexParams) {
    Node* stmt = ParseSingle("CREATE INDEX idx ON t (a, b)");
    ASSERT_NE(stmt, nullptr);
    auto* ist = static_cast<IndexStmt*>(stmt);
    ASSERT_EQ(ist->index_params.size(), 2u);

    ParseState* pstate = make_parsestate(nullptr);
    transformIndexStmt(pstate, ist);
    free_parsestate(pstate);

    // Index params should be preserved.
    EXPECT_EQ(ist->index_params.size(), 2u);
    auto* e0 = static_cast<IndexElem*>(ist->index_params[0]);
    EXPECT_EQ(e0->name, "a");
    auto* e1 = static_cast<IndexElem*>(ist->index_params[1]);
    EXPECT_EQ(e1->name, "b");
}

TEST_F(ParseUtilCmdTest, TransformIndexStmtRecordsUniquePrimary) {
    Node* stmt = ParseSingle("CREATE UNIQUE INDEX uq ON t (a)");
    ASSERT_NE(stmt, nullptr);
    auto* ist = static_cast<IndexStmt*>(stmt);
    EXPECT_TRUE(ist->unique);

    ParseState* pstate = make_parsestate(nullptr);
    transformIndexStmt(pstate, ist);
    free_parsestate(pstate);

    EXPECT_TRUE(ist->unique);
}

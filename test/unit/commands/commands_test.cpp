// commands_test.cpp — Unit tests for the commands module (M14 Task 15.13).
//
// Exercises the new commands/ handlers that ProcessUtility dispatches to:
//   sequence (CREATE SEQUENCE), view (CREATE VIEW), trigger (CREATE TRIGGER),
//   explain (EXPLAIN), dbcommands (CREATE/DROP DATABASE),
//   schemacmds (CREATE SCHEMA), tablespace (CREATE/DROP TABLESPACE),
//   functioncmds (CREATE FUNCTION/PROCEDURE).
//
// Statements are produced by the parser (raw_parser + parse_analyze) and
// dispatched through ProcessUtility, exercising the full
// parse → utility dispatch → commands handler path.

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "access/rel.hpp"
#include "catalog/bootstrap_catalog.hpp"
#include "catalog/catalog.hpp"
#include "catalog/pg_class.hpp"
#include "catalog/syscache.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "parser/analyze.hpp"
#include "parser/parsenodes.hpp"
#include "parser/parser.hpp"
#include "protocol/pqformat.hpp"
#include "protocol/utility.hpp"
#include "storage/bufmgr.hpp"
#include "storage/smgr.hpp"
#include "transaction/snapshot.hpp"
#include "transaction/transam.hpp"
#include "transaction/xact.hpp"

using pgcpp::access::InitializeRelcache;
using pgcpp::access::ResetRelcache;
using pgcpp::catalog::BootstrapCatalog;
using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_class;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::RelKind;
using pgcpp::catalog::SetCatalog;
using pgcpp::catalog::SetSysCache;
using pgcpp::catalog::SysCache;
using pgcpp::memory::AllocSetContext;
using pgcpp::parser::Node;
using pgcpp::parser::parse_analyze;
using pgcpp::parser::Query;
using pgcpp::parser::raw_parser;
using pgcpp::parser::RawStmt;
using pgcpp::protocol::CreateCommandTag;
using pgcpp::protocol::ProcessUtility;
using pgcpp::protocol::StringSink;
using pgcpp::storage::InitBufferPool;
using pgcpp::storage::SetStorageBaseDir;
using pgcpp::storage::ShutdownBufferPool;
using pgcpp::storage::smgrcloseall;
using pgcpp::transaction::BeginTransactionBlock;
using pgcpp::transaction::EndTransactionBlock;
using pgcpp::transaction::InitializeSnapshotManager;
using pgcpp::transaction::InitializeTransactionSystem;
using pgcpp::transaction::ResetTransactionState;

namespace {

class CommandsTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("commands_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        BootstrapCatalog(catalog_);
        syscache_ = new SysCache();
        SetSysCache(syscache_);

        ResetTransactionState();
        InitializeTransactionSystem();
        InitializeSnapshotManager();
        BeginTransactionBlock();

        test_dir_ = "/tmp/pgcpp_commands_test_" + std::to_string(getpid());
        SetStorageBaseDir(test_dir_);
        RunShell("rm -rf " + test_dir_);

        InitBufferPool(64);
        InitializeRelcache();
    }

    void TearDown() override {
        EndTransactionBlock();
        ResetRelcache();
        ShutdownBufferPool();
        smgrcloseall();
        RunShell("rm -rf " + test_dir_);

        SetSysCache(nullptr);
        SetCatalog(nullptr);
        delete syscache_;
        delete catalog_;

        ResetTransactionState();
        InitializeTransactionSystem();
        InitializeSnapshotManager();

        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    // Parse a SQL string and return the utility_stmt of the first Query.
    Node* ParseUtilityStmt(const std::string& sql) {
        std::vector<RawStmt*> raw = raw_parser(sql);
        if (raw.empty())
            return nullptr;
        std::vector<Query*> queries = parse_analyze(raw, sql.c_str());
        if (queries.empty())
            return nullptr;
        return queries[0]->utility_stmt;
    }

    // Run a utility statement and return the command tag.
    std::string RunUtility(const std::string& sql) {
        Node* stmt = ParseUtilityStmt(sql);
        if (stmt == nullptr)
            return "";
        return ProcessUtility(stmt, &sink_);
    }

    static void RunShell(const std::string& cmd) {
        int rc = std::system(cmd.c_str());
        (void)rc;
    }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
    SysCache* syscache_ = nullptr;
    std::string test_dir_;
    StringSink sink_;
};

// --- CREATE SEQUENCE ---

TEST_F(CommandsTest, CreateSequenceReturnsTagAndCreatesCatalogEntry) {
    EXPECT_EQ(RunUtility("CREATE SEQUENCE seq1;"), "CREATE SEQUENCE");
    auto* cat = GetCatalog();
    ASSERT_NE(cat, nullptr);
    const FormData_pg_class* row = cat->GetClassByName("seq1");
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->relkind, RelKind::kSequence);
}

TEST_F(CommandsTest, CreateSequenceIfNotExistsIsIdempotent) {
    RunUtility("CREATE SEQUENCE seq1;");
    EXPECT_EQ(RunUtility("CREATE SEQUENCE IF NOT EXISTS seq1;"), "CREATE SEQUENCE");
}

// --- CREATE VIEW ---

TEST_F(CommandsTest, CreateViewReturnsTagAndCreatesCatalogEntry) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunUtility("CREATE VIEW v1 AS SELECT a FROM t1;"), "CREATE VIEW");
    auto* cat = GetCatalog();
    ASSERT_NE(cat, nullptr);
    const FormData_pg_class* row = cat->GetClassByName("v1");
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->relkind, RelKind::kView);
}

TEST_F(CommandsTest, CreateOrReplaceViewDropsExisting) {
    RunUtility("CREATE TABLE t1 (a int4);");
    RunUtility("CREATE VIEW v1 AS SELECT a FROM t1;");
    EXPECT_EQ(RunUtility("CREATE OR REPLACE VIEW v1 AS SELECT a FROM t1;"), "CREATE VIEW");
    auto* cat = GetCatalog();
    ASSERT_NE(cat, nullptr);
    EXPECT_NE(cat->GetClassByName("v1"), nullptr);
}

// --- CREATE TRIGGER ---

TEST_F(CommandsTest, CreateTriggerSetsRelhasTriggersFlag) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunUtility("CREATE TRIGGER trg1 AFTER INSERT ON t1 EXECUTE FUNCTION "
                         "now();"),
              "CREATE TRIGGER");
    auto* cat = GetCatalog();
    ASSERT_NE(cat, nullptr);
    const FormData_pg_class* row = cat->GetClassByName("t1");
    ASSERT_NE(row, nullptr);
    EXPECT_TRUE(row->relhastriggers);
}

// --- EXPLAIN ---

TEST_F(CommandsTest, ExplainReturnsExplainTag) {
    // EXPLAIN prints to stdout; we only check the command tag here.
    testing::internal::CaptureStdout();
    EXPECT_EQ(RunUtility("EXPLAIN SELECT 1;"), "EXPLAIN");
    testing::internal::GetCapturedStdout();
}

// --- CREATE / DROP DATABASE ---

TEST_F(CommandsTest, CreateDatabaseReturnsTag) {
    EXPECT_EQ(RunUtility("CREATE DATABASE db1;"), "CREATE DATABASE");
}

TEST_F(CommandsTest, DropDatabaseReturnsTag) {
    EXPECT_EQ(RunUtility("DROP DATABASE db1;"), "DROP DATABASE");
}

TEST_F(CommandsTest, DropDatabaseIfExistsReturnsTag) {
    EXPECT_EQ(RunUtility("DROP DATABASE IF EXISTS nonexistent;"), "DROP DATABASE");
}

// --- CREATE SCHEMA ---

TEST_F(CommandsTest, CreateSchemaReturnsTag) {
    EXPECT_EQ(RunUtility("CREATE SCHEMA sch1;"), "CREATE SCHEMA");
}

// --- CREATE / DROP TABLESPACE ---

TEST_F(CommandsTest, CreateTablespaceReturnsTag) {
    EXPECT_EQ(RunUtility("CREATE TABLESPACE ts1 LOCATION '/tmp';"), "CREATE TABLESPACE");
}

TEST_F(CommandsTest, DropTablespaceReturnsTag) {
    EXPECT_EQ(RunUtility("DROP TABLESPACE ts1;"), "DROP TABLESPACE");
}

// --- CREATE FUNCTION / PROCEDURE ---

TEST_F(CommandsTest, CreateFunctionReturnsTag) {
    EXPECT_EQ(RunUtility("CREATE FUNCTION fn1() RETURNS int AS $$ SELECT 1 $$ "
                         "LANGUAGE SQL;"),
              "CREATE FUNCTION");
}

TEST_F(CommandsTest, CreateProcedureReturnsTag) {
    EXPECT_EQ(RunUtility("CREATE PROCEDURE proc1() AS $$ SELECT 1 $$ "
                         "LANGUAGE SQL;"),
              "CREATE PROCEDURE");
}

// --- CreateCommandTag for new node types ---

TEST_F(CommandsTest, CreateCommandTag_CreateSequence) {
    Node* stmt = ParseUtilityStmt("CREATE SEQUENCE seq1;");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(CreateCommandTag(stmt), "CREATE SEQUENCE");
}

TEST_F(CommandsTest, CreateCommandTag_CreateView) {
    RunUtility("CREATE TABLE t1 (a int4);");
    Node* stmt = ParseUtilityStmt("CREATE VIEW v1 AS SELECT a FROM t1;");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(CreateCommandTag(stmt), "CREATE VIEW");
}

TEST_F(CommandsTest, CreateCommandTag_CreateTrigger) {
    RunUtility("CREATE TABLE t1 (a int4);");
    Node* stmt = ParseUtilityStmt("CREATE TRIGGER trg1 AFTER INSERT ON t1 EXECUTE FUNCTION now();");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(CreateCommandTag(stmt), "CREATE TRIGGER");
}

TEST_F(CommandsTest, CreateCommandTag_Explain) {
    Node* stmt = ParseUtilityStmt("EXPLAIN SELECT 1;");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(CreateCommandTag(stmt), "EXPLAIN");
}

TEST_F(CommandsTest, CreateCommandTag_CreateDatabase) {
    Node* stmt = ParseUtilityStmt("CREATE DATABASE db1;");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(CreateCommandTag(stmt), "CREATE DATABASE");
}

TEST_F(CommandsTest, CreateCommandTag_DropDatabase) {
    Node* stmt = ParseUtilityStmt("DROP DATABASE db1;");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(CreateCommandTag(stmt), "DROP DATABASE");
}

TEST_F(CommandsTest, CreateCommandTag_CreateSchema) {
    Node* stmt = ParseUtilityStmt("CREATE SCHEMA sch1;");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(CreateCommandTag(stmt), "CREATE SCHEMA");
}

TEST_F(CommandsTest, CreateCommandTag_CreateTablespace) {
    Node* stmt = ParseUtilityStmt("CREATE TABLESPACE ts1 LOCATION '/tmp';");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(CreateCommandTag(stmt), "CREATE TABLESPACE");
}

TEST_F(CommandsTest, CreateCommandTag_DropTablespace) {
    Node* stmt = ParseUtilityStmt("DROP TABLESPACE ts1;");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(CreateCommandTag(stmt), "DROP TABLESPACE");
}

TEST_F(CommandsTest, CreateCommandTag_CreateFunction) {
    Node* stmt = ParseUtilityStmt(
        "CREATE FUNCTION fn1() RETURNS int "
        "AS $$ SELECT 1 $$ LANGUAGE SQL;");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(CreateCommandTag(stmt), "CREATE FUNCTION");
}

}  // namespace

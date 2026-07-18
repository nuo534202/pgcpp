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
#include "commands/foreigncmds.hpp"
#include "commands/policycmds.hpp"
#include "commands/publicationcmds.hpp"
#include "commands/rulecmds.hpp"
#include "commands/seclabelcmds.hpp"
#include "commands/subscriptioncmds.hpp"
#include "commands/systemcmds.hpp"
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

// P3-13: SQL language remaining items — additional using declarations.
using pgcpp::commands::AlterPolicy;
using pgcpp::commands::AlterPublication;
using pgcpp::commands::AlterRule;
using pgcpp::commands::AlterServer;
using pgcpp::commands::AlterSubscription;
using pgcpp::commands::AlterSystem;
using pgcpp::commands::CreateForeignTable;
using pgcpp::commands::CreatePolicy;
using pgcpp::commands::CreatePublication;
using pgcpp::commands::CreateRule;
using pgcpp::commands::CreateServer;
using pgcpp::commands::CreateSubscription;
using pgcpp::commands::DropPolicy;
using pgcpp::commands::DropPublication;
using pgcpp::commands::DropRule;
using pgcpp::commands::DropServer;
using pgcpp::commands::DropSubscription;
using pgcpp::commands::ImportForeignSchema;
using pgcpp::commands::SecLabel;
using pgcpp::nodes::makePallocNode;
using pgcpp::nodes::NodeTag;
using pgcpp::parser::AlterPolicyStmt;
using pgcpp::parser::AlterPublicationStmt;
using pgcpp::parser::AlterRuleStmt;
using pgcpp::parser::AlterServerStmt;
using pgcpp::parser::AlterSubscriptionStmt;
using pgcpp::parser::AlterSystemStmt;
using pgcpp::parser::CreateForeignTableStmt;
using pgcpp::parser::CreatePolicyStmt;
using pgcpp::parser::CreatePublicationStmt;
using pgcpp::parser::CreateRuleStmt;
using pgcpp::parser::CreateServerStmt;
using pgcpp::parser::CreateSubscriptionStmt;
using pgcpp::parser::DropPolicyStmt;
using pgcpp::parser::DropPublicationStmt;
using pgcpp::parser::DropRuleStmt;
using pgcpp::parser::DropServerStmt;
using pgcpp::parser::DropSubscriptionStmt;
using pgcpp::parser::ImportForeignSchemaStmt;
using pgcpp::parser::RangeVar;
using pgcpp::parser::RoleSpec;
using pgcpp::parser::SecLabelStmt;

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

    // Parse a SQL string and return the command tag without dispatching.
    std::string RunCommandTag(const std::string& sql) {
        Node* stmt = ParseUtilityStmt(sql);
        if (stmt == nullptr)
            return "";
        return CreateCommandTag(stmt);
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

// =============================================================================
// P3-13: SQL language remaining items.
//
// Covers CREATE/ALTER/DROP POLICY, PUBLICATION, SUBSCRIPTION, FOREIGN TABLE,
// SERVER, RULE, SECURITY LABEL, ALTER SYSTEM, and IMPORT FOREIGN SCHEMA.
//
// Each statement is exercised end-to-end through:
//   1. raw_parser + parse_analyze -> ProcessUtility -> command tag check
//   2. Direct handler invocation (validation + tag)
//   3. CreateCommandTag dispatch
//   4. Parser node Clone/Equals for the new statement types
//
// Handlers are skeletons (P3-3/P3-4/P3-5 wire persistence and rewriter/
// replication integration); these tests verify the parse -> dispatch -> tag
// pipeline is intact.
// =============================================================================

// -----------------------------------------------------------------------------
// 1. CREATE / ALTER / DROP POLICY
// -----------------------------------------------------------------------------

TEST_F(CommandsTest, CreatePolicyReturnsTag) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunUtility("CREATE POLICY p1 ON t1 FOR SELECT;"), "CREATE POLICY");
}

TEST_F(CommandsTest, CreatePolicyPermissiveReturnsTag) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunUtility("CREATE POLICY p1 ON t1 AS PERMISSIVE FOR SELECT;"), "CREATE POLICY");
}

TEST_F(CommandsTest, CreatePolicyRestrictiveReturnsTag) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunUtility("CREATE POLICY p1 ON t1 AS RESTRICTIVE FOR INSERT;"), "CREATE POLICY");
}

TEST_F(CommandsTest, CreatePolicyOrReplaceReturnsTag) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunUtility("CREATE OR REPLACE POLICY p1 ON t1 FOR ALL;"), "CREATE POLICY");
}

TEST_F(CommandsTest, CreatePolicyWithUsingAndCheckReturnsTag) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunUtility("CREATE POLICY p1 ON t1 FOR SELECT USING (a > 0) WITH CHECK (a > 0);"),
              "CREATE POLICY");
}

TEST_F(CommandsTest, CreatePolicyForUpdateReturnsTag) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunUtility("CREATE POLICY p1 ON t1 FOR UPDATE;"), "CREATE POLICY");
}

TEST_F(CommandsTest, CreatePolicyForDeleteReturnsTag) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunUtility("CREATE POLICY p1 ON t1 FOR DELETE;"), "CREATE POLICY");
}

TEST_F(CommandsTest, AlterPolicyReturnsTag) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunUtility("ALTER POLICY p1 ON t1 USING (a > 0);"), "ALTER POLICY");
}

TEST_F(CommandsTest, DropPolicyReturnsTag) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunUtility("DROP POLICY p1 ON t1;"), "DROP POLICY");
}

TEST_F(CommandsTest, DropPolicyIfExistsReturnsTag) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunUtility("DROP POLICY IF EXISTS p1 ON t1;"), "DROP POLICY");
}

TEST_F(CommandsTest, PolicyCommandTags) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunCommandTag("CREATE POLICY p1 ON t1 FOR SELECT;"), "CREATE POLICY");
    EXPECT_EQ(RunCommandTag("ALTER POLICY p1 ON t1 USING (a > 0);"), "ALTER POLICY");
    EXPECT_EQ(RunCommandTag("DROP POLICY p1 ON t1;"), "DROP POLICY");
}

TEST_F(CommandsTest, CreatePolicyDirectCallValidatesStmt) {
    auto* stmt = makePallocNode<CreatePolicyStmt>();
    stmt->policy_name = "p1";
    stmt->table = makePallocNode<RangeVar>();
    stmt->table->relname = "t1";
    EXPECT_EQ(CreatePolicy(stmt), "CREATE POLICY");
}

TEST_F(CommandsTest, CreatePolicyDirectCallNullStmtThrows) {
    EXPECT_THROW(CreatePolicy(nullptr), pgcpp::error::PgException);
}

TEST_F(CommandsTest, CreatePolicyDirectCallMissingNameThrows) {
    auto* stmt = makePallocNode<CreatePolicyStmt>();
    stmt->table = makePallocNode<RangeVar>();
    EXPECT_THROW(CreatePolicy(stmt), pgcpp::error::PgException);
}

TEST_F(CommandsTest, CreatePolicyDirectCallMissingTableThrows) {
    auto* stmt = makePallocNode<CreatePolicyStmt>();
    stmt->policy_name = "p1";
    EXPECT_THROW(CreatePolicy(stmt), pgcpp::error::PgException);
}

TEST_F(CommandsTest, AlterPolicyDirectCallValidatesStmt) {
    auto* stmt = makePallocNode<AlterPolicyStmt>();
    stmt->policy_name = "p1";
    stmt->table = makePallocNode<RangeVar>();
    stmt->table->relname = "t1";
    EXPECT_EQ(AlterPolicy(stmt), "ALTER POLICY");
}

TEST_F(CommandsTest, AlterPolicyDirectCallNullStmtThrows) {
    EXPECT_THROW(AlterPolicy(nullptr), pgcpp::error::PgException);
}

TEST_F(CommandsTest, DropPolicyDirectCallValidatesStmt) {
    auto* stmt = makePallocNode<DropPolicyStmt>();
    stmt->policy_name = "p1";
    stmt->table = makePallocNode<RangeVar>();
    stmt->table->relname = "t1";
    EXPECT_EQ(DropPolicy(stmt), "DROP POLICY");
}

TEST_F(CommandsTest, DropPolicyDirectCallNullStmtThrows) {
    EXPECT_THROW(DropPolicy(nullptr), pgcpp::error::PgException);
}

// -----------------------------------------------------------------------------
// 2. CREATE / ALTER / DROP PUBLICATION
// -----------------------------------------------------------------------------

TEST_F(CommandsTest, CreatePublicationReturnsTag) {
    EXPECT_EQ(RunUtility("CREATE PUBLICATION pub1;"), "CREATE PUBLICATION");
}

TEST_F(CommandsTest, CreatePublicationForAllTablesReturnsTag) {
    EXPECT_EQ(RunUtility("CREATE PUBLICATION pub1 FOR ALL TABLES;"), "CREATE PUBLICATION");
}

TEST_F(CommandsTest, CreatePublicationForTableReturnsTag) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunUtility("CREATE PUBLICATION pub1 FOR TABLE t1;"), "CREATE PUBLICATION");
}

TEST_F(CommandsTest, CreatePublicationWithOptionsReturnsTag) {
    EXPECT_EQ(RunUtility("CREATE PUBLICATION pub1 WITH (publish = 'insert,update');"),
              "CREATE PUBLICATION");
}

TEST_F(CommandsTest, AlterPublicationAddTableReturnsTag) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunUtility("ALTER PUBLICATION pub1 ADD TABLE t1;"), "ALTER PUBLICATION");
}

TEST_F(CommandsTest, AlterPublicationDropTableReturnsTag) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunUtility("ALTER PUBLICATION pub1 DROP TABLE t1;"), "ALTER PUBLICATION");
}

TEST_F(CommandsTest, AlterPublicationSetTableReturnsTag) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunUtility("ALTER PUBLICATION pub1 SET TABLE t1;"), "ALTER PUBLICATION");
}

TEST_F(CommandsTest, AlterPublicationSetOptionsReturnsTag) {
    EXPECT_EQ(RunUtility("ALTER PUBLICATION pub1 SET (publish = 'insert');"), "ALTER PUBLICATION");
}

TEST_F(CommandsTest, DropPublicationReturnsTag) {
    EXPECT_EQ(RunUtility("DROP PUBLICATION pub1;"), "DROP PUBLICATION");
}

TEST_F(CommandsTest, DropPublicationIfExistsReturnsTag) {
    EXPECT_EQ(RunUtility("DROP PUBLICATION IF EXISTS pub1;"), "DROP PUBLICATION");
}

TEST_F(CommandsTest, DropPublicationMultipleReturnsTag) {
    EXPECT_EQ(RunUtility("DROP PUBLICATION pub1, pub2;"), "DROP PUBLICATION");
}

TEST_F(CommandsTest, PublicationCommandTags) {
    EXPECT_EQ(RunCommandTag("CREATE PUBLICATION pub1;"), "CREATE PUBLICATION");
    EXPECT_EQ(RunCommandTag("ALTER PUBLICATION pub1 SET (publish = 'insert');"),
              "ALTER PUBLICATION");
    EXPECT_EQ(RunCommandTag("DROP PUBLICATION pub1;"), "DROP PUBLICATION");
}

TEST_F(CommandsTest, CreatePublicationDirectCallValidatesStmt) {
    auto* stmt = makePallocNode<CreatePublicationStmt>();
    stmt->pubname = "pub1";
    EXPECT_EQ(CreatePublication(stmt), "CREATE PUBLICATION");
}

TEST_F(CommandsTest, CreatePublicationDirectCallNullStmtThrows) {
    EXPECT_THROW(CreatePublication(nullptr), pgcpp::error::PgException);
}

TEST_F(CommandsTest, CreatePublicationDirectCallMissingNameThrows) {
    auto* stmt = makePallocNode<CreatePublicationStmt>();
    EXPECT_THROW(CreatePublication(stmt), pgcpp::error::PgException);
}

TEST_F(CommandsTest, AlterPublicationDirectCallValidatesStmt) {
    auto* stmt = makePallocNode<AlterPublicationStmt>();
    stmt->pubname = "pub1";
    EXPECT_EQ(AlterPublication(stmt), "ALTER PUBLICATION");
}

TEST_F(CommandsTest, AlterPublicationDirectCallNullStmtThrows) {
    EXPECT_THROW(AlterPublication(nullptr), pgcpp::error::PgException);
}

TEST_F(CommandsTest, DropPublicationDirectCallValidatesStmt) {
    auto* stmt = makePallocNode<DropPublicationStmt>();
    stmt->pubnames.push_back("pub1");
    EXPECT_EQ(DropPublication(stmt), "DROP PUBLICATION");
}

TEST_F(CommandsTest, DropPublicationDirectCallEmptyListThrows) {
    auto* stmt = makePallocNode<DropPublicationStmt>();
    EXPECT_THROW(DropPublication(stmt), pgcpp::error::PgException);
}

// -----------------------------------------------------------------------------
// 3. CREATE / ALTER / DROP SUBSCRIPTION
// -----------------------------------------------------------------------------

TEST_F(CommandsTest, CreateSubscriptionReturnsTag) {
    EXPECT_EQ(RunUtility("CREATE SUBSCRIPTION sub1 CONNECTION 'host=localhost' "
                         "PUBLICATION pub1;"),
              "CREATE SUBSCRIPTION");
}

TEST_F(CommandsTest, CreateSubscriptionWithOptionsReturnsTag) {
    EXPECT_EQ(RunUtility("CREATE SUBSCRIPTION sub1 CONNECTION 'host=localhost' "
                         "PUBLICATION pub1 WITH (enabled = true);"),
              "CREATE SUBSCRIPTION");
}

TEST_F(CommandsTest, AlterSubscriptionConnectionReturnsTag) {
    EXPECT_EQ(RunUtility("ALTER SUBSCRIPTION sub1 CONNECTION 'host=remote';"),
              "ALTER SUBSCRIPTION");
}

TEST_F(CommandsTest, AlterSubscriptionSetPublicationReturnsTag) {
    EXPECT_EQ(RunUtility("ALTER SUBSCRIPTION sub1 SET PUBLICATION pub1;"), "ALTER SUBSCRIPTION");
}

TEST_F(CommandsTest, AlterSubscriptionRefreshReturnsTag) {
    EXPECT_EQ(RunUtility("ALTER SUBSCRIPTION sub1 REFRESH PUBLICATION;"), "ALTER SUBSCRIPTION");
}

TEST_F(CommandsTest, AlterSubscriptionEnableReturnsTag) {
    EXPECT_EQ(RunUtility("ALTER SUBSCRIPTION sub1 ENABLE;"), "ALTER SUBSCRIPTION");
}

TEST_F(CommandsTest, AlterSubscriptionDisableReturnsTag) {
    EXPECT_EQ(RunUtility("ALTER SUBSCRIPTION sub1 DISABLE;"), "ALTER SUBSCRIPTION");
}

TEST_F(CommandsTest, AlterSubscriptionSetOptionsReturnsTag) {
    EXPECT_EQ(RunUtility("ALTER SUBSCRIPTION sub1 SET (enabled = true);"), "ALTER SUBSCRIPTION");
}

TEST_F(CommandsTest, DropSubscriptionReturnsTag) {
    EXPECT_EQ(RunUtility("DROP SUBSCRIPTION sub1;"), "DROP SUBSCRIPTION");
}

TEST_F(CommandsTest, DropSubscriptionIfExistsReturnsTag) {
    EXPECT_EQ(RunUtility("DROP SUBSCRIPTION IF EXISTS sub1;"), "DROP SUBSCRIPTION");
}

TEST_F(CommandsTest, SubscriptionCommandTags) {
    EXPECT_EQ(RunCommandTag("CREATE SUBSCRIPTION sub1 CONNECTION 'host=localhost' "
                            "PUBLICATION pub1;"),
              "CREATE SUBSCRIPTION");
    EXPECT_EQ(RunCommandTag("ALTER SUBSCRIPTION sub1 ENABLE;"), "ALTER SUBSCRIPTION");
    EXPECT_EQ(RunCommandTag("DROP SUBSCRIPTION sub1;"), "DROP SUBSCRIPTION");
}

TEST_F(CommandsTest, CreateSubscriptionDirectCallValidatesStmt) {
    auto* stmt = makePallocNode<CreateSubscriptionStmt>();
    stmt->subname = "sub1";
    stmt->conninfo = "host=localhost";
    stmt->publications.push_back("pub1");
    EXPECT_EQ(CreateSubscription(stmt), "CREATE SUBSCRIPTION");
}

TEST_F(CommandsTest, CreateSubscriptionDirectCallNullStmtThrows) {
    EXPECT_THROW(CreateSubscription(nullptr), pgcpp::error::PgException);
}

TEST_F(CommandsTest, CreateSubscriptionDirectCallMissingConninfoThrows) {
    auto* stmt = makePallocNode<CreateSubscriptionStmt>();
    stmt->subname = "sub1";
    EXPECT_THROW(CreateSubscription(stmt), pgcpp::error::PgException);
}

TEST_F(CommandsTest, CreateSubscriptionDirectCallMissingPublicationsThrows) {
    auto* stmt = makePallocNode<CreateSubscriptionStmt>();
    stmt->subname = "sub1";
    stmt->conninfo = "host=localhost";
    EXPECT_THROW(CreateSubscription(stmt), pgcpp::error::PgException);
}

TEST_F(CommandsTest, AlterSubscriptionDirectCallValidatesStmt) {
    auto* stmt = makePallocNode<AlterSubscriptionStmt>();
    stmt->subname = "sub1";
    EXPECT_EQ(AlterSubscription(stmt), "ALTER SUBSCRIPTION");
}

TEST_F(CommandsTest, DropSubscriptionDirectCallValidatesStmt) {
    auto* stmt = makePallocNode<DropSubscriptionStmt>();
    stmt->subname = "sub1";
    EXPECT_EQ(DropSubscription(stmt), "DROP SUBSCRIPTION");
}

TEST_F(CommandsTest, DropSubscriptionDirectCallMissingNameThrows) {
    auto* stmt = makePallocNode<DropSubscriptionStmt>();
    EXPECT_THROW(DropSubscription(stmt), pgcpp::error::PgException);
}

// -----------------------------------------------------------------------------
// 4. CREATE FOREIGN TABLE / SERVER / IMPORT FOREIGN SCHEMA
// -----------------------------------------------------------------------------

TEST_F(CommandsTest, CreateForeignTableReturnsTag) {
    EXPECT_EQ(RunUtility("CREATE FOREIGN TABLE ft1 (a int4) SERVER fs1;"), "CREATE FOREIGN TABLE");
}

TEST_F(CommandsTest, CreateForeignTableIfNotExistsReturnsTag) {
    EXPECT_EQ(RunUtility("CREATE FOREIGN TABLE IF NOT EXISTS ft1 (a int4) SERVER fs1;"),
              "CREATE FOREIGN TABLE");
}

TEST_F(CommandsTest, CreateForeignTableWithOptionsReturnsTag) {
    EXPECT_EQ(RunUtility("CREATE FOREIGN TABLE ft1 (a int4) SERVER fs1 "
                         "OPTIONS (schema_name = 'public');"),
              "CREATE FOREIGN TABLE");
}

TEST_F(CommandsTest, CreateServerReturnsTag) {
    EXPECT_EQ(RunUtility("CREATE SERVER s1 FOREIGN DATA WRAPPER fdw1;"), "CREATE SERVER");
}

TEST_F(CommandsTest, CreateServerIfNotExistsReturnsTag) {
    EXPECT_EQ(RunUtility("CREATE SERVER IF NOT EXISTS s1 FOREIGN DATA WRAPPER fdw1;"),
              "CREATE SERVER");
}

TEST_F(CommandsTest, CreateServerWithTypeReturnsTag) {
    EXPECT_EQ(RunUtility("CREATE SERVER s1 TYPE 'postgres' FOREIGN DATA WRAPPER fdw1;"),
              "CREATE SERVER");
}

TEST_F(CommandsTest, CreateServerWithVersionReturnsTag) {
    EXPECT_EQ(RunUtility("CREATE SERVER s1 VERSION '15.0' FOREIGN DATA WRAPPER fdw1;"),
              "CREATE SERVER");
}

TEST_F(CommandsTest, CreateServerWithOptionsReturnsTag) {
    EXPECT_EQ(RunUtility("CREATE SERVER s1 FOREIGN DATA WRAPPER fdw1 "
                         "OPTIONS (host = 'localhost');"),
              "CREATE SERVER");
}

TEST_F(CommandsTest, AlterServerOptionsReturnsTag) {
    EXPECT_EQ(RunUtility("ALTER SERVER s1 OPTIONS (host = 'remote');"), "ALTER SERVER");
}

TEST_F(CommandsTest, AlterServerVersionReturnsTag) {
    EXPECT_EQ(RunUtility("ALTER SERVER s1 VERSION '15.1';"), "ALTER SERVER");
}

TEST_F(CommandsTest, AlterServerOwnerReturnsTag) {
    EXPECT_EQ(RunUtility("ALTER SERVER s1 OWNER TO role1;"), "ALTER SERVER");
}

TEST_F(CommandsTest, DropServerReturnsTag) {
    EXPECT_EQ(RunUtility("DROP SERVER s1;"), "DROP SERVER");
}

TEST_F(CommandsTest, DropServerIfExistsReturnsTag) {
    EXPECT_EQ(RunUtility("DROP SERVER IF EXISTS s1;"), "DROP SERVER");
}

TEST_F(CommandsTest, ImportForeignSchemaReturnsTag) {
    EXPECT_EQ(RunUtility("IMPORT FOREIGN SCHEMA remote_schema FROM SERVER s1 INTO local_schema;"),
              "IMPORT FOREIGN SCHEMA");
}

TEST_F(CommandsTest, ImportForeignSchemaLimitToReturnsTag) {
    EXPECT_EQ(RunUtility("IMPORT FOREIGN SCHEMA remote_schema LIMIT TO (t1, t2) "
                         "FROM SERVER s1 INTO local_schema;"),
              "IMPORT FOREIGN SCHEMA");
}

TEST_F(CommandsTest, ImportForeignSchemaExceptReturnsTag) {
    EXPECT_EQ(RunUtility("IMPORT FOREIGN SCHEMA remote_schema EXCEPT (t1) "
                         "FROM SERVER s1 INTO local_schema;"),
              "IMPORT FOREIGN SCHEMA");
}

TEST_F(CommandsTest, ForeignServerCommandTags) {
    EXPECT_EQ(RunCommandTag("CREATE FOREIGN TABLE ft1 (a int4) SERVER fs1;"),
              "CREATE FOREIGN TABLE");
    EXPECT_EQ(RunCommandTag("CREATE SERVER s1 FOREIGN DATA WRAPPER fdw1;"), "CREATE SERVER");
    EXPECT_EQ(RunCommandTag("ALTER SERVER s1 OWNER TO role1;"), "ALTER SERVER");
    EXPECT_EQ(RunCommandTag("DROP SERVER s1;"), "DROP SERVER");
    EXPECT_EQ(RunCommandTag("IMPORT FOREIGN SCHEMA r FROM SERVER s1 INTO l;"),
              "IMPORT FOREIGN SCHEMA");
}

TEST_F(CommandsTest, CreateForeignTableDirectCallValidatesStmt) {
    auto* stmt = makePallocNode<CreateForeignTableStmt>();
    stmt->relation = makePallocNode<RangeVar>();
    stmt->relation->relname = "ft1";
    stmt->servername = "fs1";
    EXPECT_EQ(CreateForeignTable(stmt), "CREATE FOREIGN TABLE");
}

TEST_F(CommandsTest, CreateForeignTableDirectCallMissingRelationThrows) {
    auto* stmt = makePallocNode<CreateForeignTableStmt>();
    stmt->servername = "fs1";
    EXPECT_THROW(CreateForeignTable(stmt), pgcpp::error::PgException);
}

TEST_F(CommandsTest, CreateForeignTableDirectCallMissingServerThrows) {
    auto* stmt = makePallocNode<CreateForeignTableStmt>();
    stmt->relation = makePallocNode<RangeVar>();
    stmt->relation->relname = "ft1";
    EXPECT_THROW(CreateForeignTable(stmt), pgcpp::error::PgException);
}

TEST_F(CommandsTest, CreateServerDirectCallValidatesStmt) {
    auto* stmt = makePallocNode<CreateServerStmt>();
    stmt->servername = "s1";
    stmt->fdwname = "fdw1";
    EXPECT_EQ(CreateServer(stmt), "CREATE SERVER");
}

TEST_F(CommandsTest, CreateServerDirectCallMissingNameThrows) {
    auto* stmt = makePallocNode<CreateServerStmt>();
    stmt->fdwname = "fdw1";
    EXPECT_THROW(CreateServer(stmt), pgcpp::error::PgException);
}

TEST_F(CommandsTest, CreateServerDirectCallMissingFdwThrows) {
    auto* stmt = makePallocNode<CreateServerStmt>();
    stmt->servername = "s1";
    EXPECT_THROW(CreateServer(stmt), pgcpp::error::PgException);
}

TEST_F(CommandsTest, AlterServerDirectCallValidatesStmt) {
    auto* stmt = makePallocNode<AlterServerStmt>();
    stmt->servername = "s1";
    EXPECT_EQ(AlterServer(stmt), "ALTER SERVER");
}

TEST_F(CommandsTest, DropServerDirectCallValidatesStmt) {
    auto* stmt = makePallocNode<DropServerStmt>();
    stmt->servernames.push_back("s1");
    EXPECT_EQ(DropServer(stmt), "DROP SERVER");
}

TEST_F(CommandsTest, DropServerDirectCallEmptyListThrows) {
    auto* stmt = makePallocNode<DropServerStmt>();
    EXPECT_THROW(DropServer(stmt), pgcpp::error::PgException);
}

TEST_F(CommandsTest, ImportForeignSchemaDirectCallValidatesStmt) {
    auto* stmt = makePallocNode<ImportForeignSchemaStmt>();
    stmt->remote_schema = "remote";
    stmt->server_name = "s1";
    stmt->local_schema = "local";
    EXPECT_EQ(ImportForeignSchema(stmt), "IMPORT FOREIGN SCHEMA");
}

TEST_F(CommandsTest, ImportForeignSchemaDirectCallMissingRemoteThrows) {
    auto* stmt = makePallocNode<ImportForeignSchemaStmt>();
    stmt->server_name = "s1";
    stmt->local_schema = "local";
    EXPECT_THROW(ImportForeignSchema(stmt), pgcpp::error::PgException);
}

TEST_F(CommandsTest, ImportForeignSchemaDirectCallMissingServerThrows) {
    auto* stmt = makePallocNode<ImportForeignSchemaStmt>();
    stmt->remote_schema = "remote";
    stmt->local_schema = "local";
    EXPECT_THROW(ImportForeignSchema(stmt), pgcpp::error::PgException);
}

TEST_F(CommandsTest, ImportForeignSchemaDirectCallMissingLocalThrows) {
    auto* stmt = makePallocNode<ImportForeignSchemaStmt>();
    stmt->remote_schema = "remote";
    stmt->server_name = "s1";
    EXPECT_THROW(ImportForeignSchema(stmt), pgcpp::error::PgException);
}

// -----------------------------------------------------------------------------
// 5. CREATE / ALTER / DROP RULE
// -----------------------------------------------------------------------------

TEST_F(CommandsTest, CreateRuleDoNothingReturnsTag) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunUtility("CREATE RULE r1 AS ON SELECT TO t1 DO INSTEAD NOTHING;"), "CREATE RULE");
}

TEST_F(CommandsTest, CreateRuleOnInsertReturnsTag) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunUtility("CREATE RULE r1 AS ON INSERT TO t1 DO INSTEAD NOTHING;"), "CREATE RULE");
}

TEST_F(CommandsTest, CreateRuleOnUpdateReturnsTag) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunUtility("CREATE RULE r1 AS ON UPDATE TO t1 DO NOTHING;"), "CREATE RULE");
}

TEST_F(CommandsTest, CreateRuleOnDeleteReturnsTag) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunUtility("CREATE RULE r1 AS ON DELETE TO t1 DO INSTEAD NOTHING;"), "CREATE RULE");
}

TEST_F(CommandsTest, CreateRuleOrReplaceReturnsTag) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunUtility("CREATE OR REPLACE RULE r1 AS ON SELECT TO t1 DO INSTEAD NOTHING;"),
              "CREATE RULE");
}

TEST_F(CommandsTest, AlterRuleDoNothingReturnsTag) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunUtility("ALTER RULE r1 ON t1 DO NOTHING;"), "ALTER RULE");
}

TEST_F(CommandsTest, DropRuleReturnsTag) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunUtility("DROP RULE r1 ON t1;"), "DROP RULE");
}

TEST_F(CommandsTest, DropRuleIfExistsReturnsTag) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunUtility("DROP RULE IF EXISTS r1 ON t1;"), "DROP RULE");
}

TEST_F(CommandsTest, RuleCommandTags) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunCommandTag("CREATE RULE r1 AS ON SELECT TO t1 DO INSTEAD NOTHING;"),
              "CREATE RULE");
    EXPECT_EQ(RunCommandTag("ALTER RULE r1 ON t1 DO NOTHING;"), "ALTER RULE");
    EXPECT_EQ(RunCommandTag("DROP RULE r1 ON t1;"), "DROP RULE");
}

TEST_F(CommandsTest, CreateRuleDirectCallValidatesStmt) {
    auto* stmt = makePallocNode<CreateRuleStmt>();
    stmt->rule_name = "r1";
    stmt->relation = makePallocNode<RangeVar>();
    stmt->relation->relname = "t1";
    EXPECT_EQ(CreateRule(stmt), "CREATE RULE");
}

TEST_F(CommandsTest, CreateRuleDirectCallMissingNameThrows) {
    auto* stmt = makePallocNode<CreateRuleStmt>();
    stmt->relation = makePallocNode<RangeVar>();
    stmt->relation->relname = "t1";
    EXPECT_THROW(CreateRule(stmt), pgcpp::error::PgException);
}

TEST_F(CommandsTest, CreateRuleDirectCallMissingRelationThrows) {
    auto* stmt = makePallocNode<CreateRuleStmt>();
    stmt->rule_name = "r1";
    EXPECT_THROW(CreateRule(stmt), pgcpp::error::PgException);
}

TEST_F(CommandsTest, AlterRuleDirectCallValidatesStmt) {
    auto* stmt = makePallocNode<AlterRuleStmt>();
    stmt->rule_name = "r1";
    stmt->relation = makePallocNode<RangeVar>();
    stmt->relation->relname = "t1";
    EXPECT_EQ(AlterRule(stmt), "ALTER RULE");
}

TEST_F(CommandsTest, DropRuleDirectCallValidatesStmt) {
    auto* stmt = makePallocNode<DropRuleStmt>();
    stmt->rule_names.push_back("r1");
    stmt->relation = makePallocNode<RangeVar>();
    stmt->relation->relname = "t1";
    EXPECT_EQ(DropRule(stmt), "DROP RULE");
}

TEST_F(CommandsTest, DropRuleDirectCallMissingNamesThrows) {
    auto* stmt = makePallocNode<DropRuleStmt>();
    stmt->relation = makePallocNode<RangeVar>();
    stmt->relation->relname = "t1";
    EXPECT_THROW(DropRule(stmt), pgcpp::error::PgException);
}

// -----------------------------------------------------------------------------
// 6. SECURITY LABEL
// -----------------------------------------------------------------------------

TEST_F(CommandsTest, SecLabelReturnsTag) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunUtility("SECURITY LABEL ON t1 IS 'classified';"), "SECURITY LABEL");
}

TEST_F(CommandsTest, SecLabelWithProviderReturnsTag) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunUtility("SECURITY LABEL FOR selinux ON t1 IS 'unlabeled';"), "SECURITY LABEL");
}

TEST_F(CommandsTest, SecLabelNullRemovesReturnsTag) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunUtility("SECURITY LABEL ON t1 IS NULL;"), "SECURITY LABEL");
}

TEST_F(CommandsTest, SecLabelCommandTag) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunCommandTag("SECURITY LABEL ON t1 IS 'classified';"), "SECURITY LABEL");
}

TEST_F(CommandsTest, SecLabelDirectCallValidatesStmt) {
    auto* stmt = makePallocNode<SecLabelStmt>();
    stmt->label = "classified";
    // Push a fake object node so object is non-empty.
    stmt->object.push_back(nullptr);  // NOLINT: any non-empty vector passes
    EXPECT_EQ(SecLabel(stmt), "SECURITY LABEL");
}

TEST_F(CommandsTest, SecLabelDirectCallNullStmtThrows) {
    EXPECT_THROW(SecLabel(nullptr), pgcpp::error::PgException);
}

TEST_F(CommandsTest, SecLabelDirectCallEmptyObjectThrows) {
    auto* stmt = makePallocNode<SecLabelStmt>();
    stmt->label = "classified";
    EXPECT_THROW(SecLabel(stmt), pgcpp::error::PgException);
}

// -----------------------------------------------------------------------------
// 7. ALTER SYSTEM
// -----------------------------------------------------------------------------

TEST_F(CommandsTest, AlterSystemSetReturnsTag) {
    EXPECT_EQ(RunUtility("ALTER SYSTEM SET shared_buffers = '128MB';"), "ALTER SYSTEM");
}

TEST_F(CommandsTest, AlterSystemResetReturnsTag) {
    EXPECT_EQ(RunUtility("ALTER SYSTEM RESET shared_buffers;"), "ALTER SYSTEM");
}

TEST_F(CommandsTest, AlterSystemResetAllReturnsTag) {
    EXPECT_EQ(RunUtility("ALTER SYSTEM RESET ALL;"), "ALTER SYSTEM");
}

TEST_F(CommandsTest, AlterSystemCommandTags) {
    EXPECT_EQ(RunCommandTag("ALTER SYSTEM SET shared_buffers = '128MB';"), "ALTER SYSTEM");
    EXPECT_EQ(RunCommandTag("ALTER SYSTEM RESET shared_buffers;"), "ALTER SYSTEM");
    EXPECT_EQ(RunCommandTag("ALTER SYSTEM RESET ALL;"), "ALTER SYSTEM");
}

TEST_F(CommandsTest, AlterSystemDirectCallSetValidatesStmt) {
    auto* stmt = makePallocNode<AlterSystemStmt>();
    stmt->kind = AlterSystemStmt::Kind::kSet;
    stmt->name = "shared_buffers";
    EXPECT_EQ(AlterSystem(stmt), "ALTER SYSTEM");
}

TEST_F(CommandsTest, AlterSystemDirectCallResetValidatesStmt) {
    auto* stmt = makePallocNode<AlterSystemStmt>();
    stmt->kind = AlterSystemStmt::Kind::kReset;
    stmt->name = "shared_buffers";
    EXPECT_EQ(AlterSystem(stmt), "ALTER SYSTEM");
}

TEST_F(CommandsTest, AlterSystemDirectCallResetAllValidatesStmt) {
    auto* stmt = makePallocNode<AlterSystemStmt>();
    stmt->kind = AlterSystemStmt::Kind::kResetAll;
    // No name required for RESET ALL.
    EXPECT_EQ(AlterSystem(stmt), "ALTER SYSTEM");
}

TEST_F(CommandsTest, AlterSystemDirectCallNullStmtThrows) {
    EXPECT_THROW(AlterSystem(nullptr), pgcpp::error::PgException);
}

TEST_F(CommandsTest, AlterSystemDirectCallMissingNameThrows) {
    auto* stmt = makePallocNode<AlterSystemStmt>();
    stmt->kind = AlterSystemStmt::Kind::kSet;
    // name is empty.
    EXPECT_THROW(AlterSystem(stmt), pgcpp::error::PgException);
}

// -----------------------------------------------------------------------------
// 8. Node Clone/Equals for new parse tree node types
// -----------------------------------------------------------------------------

TEST_F(CommandsTest, CreatePolicyStmt_Clone) {
    auto* orig = makePallocNode<CreatePolicyStmt>();
    orig->policy_name = "p1";
    orig->table = makePallocNode<RangeVar>();
    orig->table->relname = "t1";
    orig->permissive = false;
    orig->replace = true;

    Node* copy = orig->Clone();
    ASSERT_NE(copy, nullptr);
    ASSERT_EQ(copy->GetTag(), orig->GetTag());
    auto* cloned = static_cast<CreatePolicyStmt*>(copy);
    EXPECT_EQ(cloned->policy_name, orig->policy_name);
    EXPECT_NE(cloned->table, nullptr);
    EXPECT_EQ(cloned->table->relname, "t1");
    EXPECT_EQ(cloned->permissive, orig->permissive);
    EXPECT_EQ(cloned->replace, orig->replace);
}

TEST_F(CommandsTest, CreatePolicyStmt_Equals) {
    auto* a = makePallocNode<CreatePolicyStmt>();
    a->policy_name = "p1";
    a->permissive = true;
    auto* b = makePallocNode<CreatePolicyStmt>();
    b->policy_name = "p1";
    b->permissive = true;
    EXPECT_TRUE(a->Equals(*b));
}

TEST_F(CommandsTest, CreatePolicyStmt_NotEqualsDifferentName) {
    auto* a = makePallocNode<CreatePolicyStmt>();
    a->policy_name = "p1";
    auto* b = makePallocNode<CreatePolicyStmt>();
    b->policy_name = "p2";
    EXPECT_FALSE(a->Equals(*b));
}

TEST_F(CommandsTest, CreatePolicyStmt_NotEqualsDifferentTag) {
    auto* a = makePallocNode<CreatePolicyStmt>();
    a->policy_name = "p1";
    auto* b = makePallocNode<DropPolicyStmt>();
    b->policy_name = "p1";
    EXPECT_FALSE(a->Equals(*b));
}

TEST_F(CommandsTest, AlterPolicyStmt_Clone) {
    auto* orig = makePallocNode<AlterPolicyStmt>();
    orig->policy_name = "p1";
    orig->table = makePallocNode<RangeVar>();
    orig->table->relname = "t1";

    Node* copy = orig->Clone();
    ASSERT_NE(copy, nullptr);
    auto* cloned = static_cast<AlterPolicyStmt*>(copy);
    EXPECT_EQ(cloned->policy_name, "p1");
    EXPECT_NE(cloned->table, nullptr);
}

TEST_F(CommandsTest, DropPolicyStmt_Clone) {
    auto* orig = makePallocNode<DropPolicyStmt>();
    orig->policy_name = "p1";
    orig->missing_ok = true;
    orig->behavior = pgcpp::parser::DropBehavior::kCascade;

    Node* copy = orig->Clone();
    auto* cloned = static_cast<DropPolicyStmt*>(copy);
    EXPECT_EQ(cloned->policy_name, "p1");
    EXPECT_TRUE(cloned->missing_ok);
    EXPECT_EQ(cloned->behavior, pgcpp::parser::DropBehavior::kCascade);
}

TEST_F(CommandsTest, CreatePublicationStmt_Clone) {
    auto* orig = makePallocNode<CreatePublicationStmt>();
    orig->pubname = "pub1";
    orig->for_all_tables = true;

    Node* copy = orig->Clone();
    auto* cloned = static_cast<CreatePublicationStmt*>(copy);
    EXPECT_EQ(cloned->pubname, "pub1");
    EXPECT_TRUE(cloned->for_all_tables);
}

TEST_F(CommandsTest, CreatePublicationStmt_Equals) {
    auto* a = makePallocNode<CreatePublicationStmt>();
    a->pubname = "pub1";
    a->for_all_tables = true;
    auto* b = makePallocNode<CreatePublicationStmt>();
    b->pubname = "pub1";
    b->for_all_tables = true;
    EXPECT_TRUE(a->Equals(*b));
}

TEST_F(CommandsTest, AlterPublicationStmt_Clone) {
    auto* orig = makePallocNode<AlterPublicationStmt>();
    orig->pubname = "pub1";
    orig->action = AlterPublicationStmt::Action::kDrop;

    Node* copy = orig->Clone();
    auto* cloned = static_cast<AlterPublicationStmt*>(copy);
    EXPECT_EQ(cloned->pubname, "pub1");
    EXPECT_EQ(cloned->action, AlterPublicationStmt::Action::kDrop);
}

TEST_F(CommandsTest, DropPublicationStmt_Clone) {
    auto* orig = makePallocNode<DropPublicationStmt>();
    orig->pubnames = {"pub1", "pub2"};
    orig->missing_ok = true;

    Node* copy = orig->Clone();
    auto* cloned = static_cast<DropPublicationStmt*>(copy);
    EXPECT_EQ(cloned->pubnames, orig->pubnames);
    EXPECT_TRUE(cloned->missing_ok);
}

TEST_F(CommandsTest, CreateSubscriptionStmt_Clone) {
    auto* orig = makePallocNode<CreateSubscriptionStmt>();
    orig->subname = "sub1";
    orig->conninfo = "host=localhost";
    orig->publications = {"pub1", "pub2"};

    Node* copy = orig->Clone();
    auto* cloned = static_cast<CreateSubscriptionStmt*>(copy);
    EXPECT_EQ(cloned->subname, "sub1");
    EXPECT_EQ(cloned->conninfo, "host=localhost");
    EXPECT_EQ(cloned->publications, orig->publications);
}

TEST_F(CommandsTest, AlterSubscriptionStmt_Clone) {
    auto* orig = makePallocNode<AlterSubscriptionStmt>();
    orig->subname = "sub1";
    orig->kind = AlterSubscriptionStmt::Kind::kEnable;

    Node* copy = orig->Clone();
    auto* cloned = static_cast<AlterSubscriptionStmt*>(copy);
    EXPECT_EQ(cloned->subname, "sub1");
    EXPECT_EQ(cloned->kind, AlterSubscriptionStmt::Kind::kEnable);
}

TEST_F(CommandsTest, DropSubscriptionStmt_Clone) {
    auto* orig = makePallocNode<DropSubscriptionStmt>();
    orig->subname = "sub1";
    orig->missing_ok = true;

    Node* copy = orig->Clone();
    auto* cloned = static_cast<DropSubscriptionStmt*>(copy);
    EXPECT_EQ(cloned->subname, "sub1");
    EXPECT_TRUE(cloned->missing_ok);
}

TEST_F(CommandsTest, CreateForeignTableStmt_Clone) {
    auto* orig = makePallocNode<CreateForeignTableStmt>();
    orig->relation = makePallocNode<RangeVar>();
    orig->relation->relname = "ft1";
    orig->servername = "fs1";
    orig->if_not_exists = true;

    Node* copy = orig->Clone();
    auto* cloned = static_cast<CreateForeignTableStmt*>(copy);
    EXPECT_EQ(cloned->servername, "fs1");
    EXPECT_TRUE(cloned->if_not_exists);
    ASSERT_NE(cloned->relation, nullptr);
    EXPECT_EQ(cloned->relation->relname, "ft1");
}

TEST_F(CommandsTest, CreateServerStmt_Clone) {
    auto* orig = makePallocNode<CreateServerStmt>();
    orig->servername = "s1";
    orig->servertype = "postgres";
    orig->fdwname = "fdw1";
    orig->if_not_exists = true;

    Node* copy = orig->Clone();
    auto* cloned = static_cast<CreateServerStmt*>(copy);
    EXPECT_EQ(cloned->servername, "s1");
    EXPECT_EQ(cloned->servertype, "postgres");
    EXPECT_EQ(cloned->fdwname, "fdw1");
    EXPECT_TRUE(cloned->if_not_exists);
}

TEST_F(CommandsTest, CreateServerStmt_Equals) {
    auto* a = makePallocNode<CreateServerStmt>();
    a->servername = "s1";
    a->fdwname = "fdw1";
    auto* b = makePallocNode<CreateServerStmt>();
    b->servername = "s1";
    b->fdwname = "fdw1";
    EXPECT_TRUE(a->Equals(*b));
}

TEST_F(CommandsTest, CreateServerStmt_NotEqualsDifferentFdw) {
    auto* a = makePallocNode<CreateServerStmt>();
    a->servername = "s1";
    a->fdwname = "fdw1";
    auto* b = makePallocNode<CreateServerStmt>();
    b->servername = "s1";
    b->fdwname = "fdw2";
    EXPECT_FALSE(a->Equals(*b));
}

TEST_F(CommandsTest, AlterServerStmt_Clone) {
    auto* orig = makePallocNode<AlterServerStmt>();
    orig->servername = "s1";
    orig->kind = AlterServerStmt::Kind::kChangeOwner;
    orig->version = "15.0";
    orig->has_version = true;

    Node* copy = orig->Clone();
    auto* cloned = static_cast<AlterServerStmt*>(copy);
    EXPECT_EQ(cloned->servername, "s1");
    EXPECT_EQ(cloned->kind, AlterServerStmt::Kind::kChangeOwner);
    EXPECT_EQ(cloned->version, "15.0");
    EXPECT_TRUE(cloned->has_version);
}

TEST_F(CommandsTest, DropServerStmt_Clone) {
    auto* orig = makePallocNode<DropServerStmt>();
    orig->servernames = {"s1", "s2"};
    orig->missing_ok = true;
    orig->behavior = pgcpp::parser::DropBehavior::kCascade;

    Node* copy = orig->Clone();
    auto* cloned = static_cast<DropServerStmt*>(copy);
    EXPECT_EQ(cloned->servernames, orig->servernames);
    EXPECT_TRUE(cloned->missing_ok);
    EXPECT_EQ(cloned->behavior, pgcpp::parser::DropBehavior::kCascade);
}

TEST_F(CommandsTest, CreateRuleStmt_Clone) {
    auto* orig = makePallocNode<CreateRuleStmt>();
    orig->replace = true;
    orig->rule_name = "r1";
    orig->relation = makePallocNode<RangeVar>();
    orig->relation->relname = "t1";
    orig->event = 1;  // CMD_SELECT
    orig->instead = true;
    orig->nothing = true;

    Node* copy = orig->Clone();
    auto* cloned = static_cast<CreateRuleStmt*>(copy);
    EXPECT_TRUE(cloned->replace);
    EXPECT_EQ(cloned->rule_name, "r1");
    ASSERT_NE(cloned->relation, nullptr);
    EXPECT_EQ(cloned->relation->relname, "t1");
    EXPECT_EQ(cloned->event, 1);
    EXPECT_TRUE(cloned->instead);
    EXPECT_TRUE(cloned->nothing);
}

TEST_F(CommandsTest, AlterRuleStmt_Clone) {
    auto* orig = makePallocNode<AlterRuleStmt>();
    orig->rule_name = "r1";
    orig->relation = makePallocNode<RangeVar>();
    orig->relation->relname = "t1";
    orig->nothing = true;

    Node* copy = orig->Clone();
    auto* cloned = static_cast<AlterRuleStmt*>(copy);
    EXPECT_EQ(cloned->rule_name, "r1");
    ASSERT_NE(cloned->relation, nullptr);
    EXPECT_EQ(cloned->relation->relname, "t1");
    EXPECT_TRUE(cloned->nothing);
}

TEST_F(CommandsTest, DropRuleStmt_Clone) {
    auto* orig = makePallocNode<DropRuleStmt>();
    orig->rule_names = {"r1", "r2"};
    orig->relation = makePallocNode<RangeVar>();
    orig->relation->relname = "t1";
    orig->missing_ok = true;

    Node* copy = orig->Clone();
    auto* cloned = static_cast<DropRuleStmt*>(copy);
    EXPECT_EQ(cloned->rule_names, orig->rule_names);
    ASSERT_NE(cloned->relation, nullptr);
    EXPECT_EQ(cloned->relation->relname, "t1");
    EXPECT_TRUE(cloned->missing_ok);
}

TEST_F(CommandsTest, SecLabelStmt_Clone) {
    auto* orig = makePallocNode<SecLabelStmt>();
    orig->provider = "selinux";
    orig->objtype = pgcpp::parser::ObjectType::kTable;
    orig->label = "classified";

    Node* copy = orig->Clone();
    auto* cloned = static_cast<SecLabelStmt*>(copy);
    EXPECT_EQ(cloned->provider, "selinux");
    EXPECT_EQ(cloned->objtype, pgcpp::parser::ObjectType::kTable);
    EXPECT_EQ(cloned->label, "classified");
}

TEST_F(CommandsTest, AlterSystemStmt_Clone) {
    auto* orig = makePallocNode<AlterSystemStmt>();
    orig->name = "shared_buffers";
    orig->kind = AlterSystemStmt::Kind::kReset;

    Node* copy = orig->Clone();
    auto* cloned = static_cast<AlterSystemStmt*>(copy);
    EXPECT_EQ(cloned->name, "shared_buffers");
    EXPECT_EQ(cloned->kind, AlterSystemStmt::Kind::kReset);
}

TEST_F(CommandsTest, AlterSystemStmt_Equals) {
    auto* a = makePallocNode<AlterSystemStmt>();
    a->name = "shared_buffers";
    a->kind = AlterSystemStmt::Kind::kSet;
    auto* b = makePallocNode<AlterSystemStmt>();
    b->name = "shared_buffers";
    b->kind = AlterSystemStmt::Kind::kSet;
    EXPECT_TRUE(a->Equals(*b));
}

TEST_F(CommandsTest, AlterSystemStmt_NotEqualsDifferentKind) {
    auto* a = makePallocNode<AlterSystemStmt>();
    a->name = "shared_buffers";
    a->kind = AlterSystemStmt::Kind::kSet;
    auto* b = makePallocNode<AlterSystemStmt>();
    b->name = "shared_buffers";
    b->kind = AlterSystemStmt::Kind::kReset;
    EXPECT_FALSE(a->Equals(*b));
}

TEST_F(CommandsTest, ImportForeignSchemaStmt_Clone) {
    auto* orig = makePallocNode<ImportForeignSchemaStmt>();
    orig->remote_schema = "remote";
    orig->local_schema = "local";
    orig->server_name = "s1";
    orig->kind = ImportForeignSchemaStmt::Kind::kExcept;

    Node* copy = orig->Clone();
    auto* cloned = static_cast<ImportForeignSchemaStmt*>(copy);
    EXPECT_EQ(cloned->remote_schema, "remote");
    EXPECT_EQ(cloned->local_schema, "local");
    EXPECT_EQ(cloned->server_name, "s1");
    EXPECT_EQ(cloned->kind, ImportForeignSchemaStmt::Kind::kExcept);
}

TEST_F(CommandsTest, ImportForeignSchemaStmt_Equals) {
    auto* a = makePallocNode<ImportForeignSchemaStmt>();
    a->remote_schema = "remote";
    a->server_name = "s1";
    a->local_schema = "local";
    auto* b = makePallocNode<ImportForeignSchemaStmt>();
    b->remote_schema = "remote";
    b->server_name = "s1";
    b->local_schema = "local";
    EXPECT_TRUE(a->Equals(*b));
}

TEST_F(CommandsTest, ImportForeignSchemaStmt_NotEqualsDifferentServer) {
    auto* a = makePallocNode<ImportForeignSchemaStmt>();
    a->server_name = "s1";
    auto* b = makePallocNode<ImportForeignSchemaStmt>();
    b->server_name = "s2";
    EXPECT_FALSE(a->Equals(*b));
}

// -----------------------------------------------------------------------------
// 9. Edge cases: CreateCommandTag for null/unknown statements
// -----------------------------------------------------------------------------

TEST_F(CommandsTest, CreateCommandTagNullReturnsEmpty) {
    EXPECT_EQ(CreateCommandTag(nullptr), "");
}

TEST_F(CommandsTest, ProcessUtilityNullReturnsEmpty) {
    EXPECT_EQ(ProcessUtility(nullptr, &sink_), "");
}

}  // namespace

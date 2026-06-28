// utility_test.cpp — Unit tests for ProcessUtility (M11 Phase 15.9).
//
// Tests the ProcessUtility dispatch for utility statements:
//   TransactionStmt (BEGIN/COMMIT/ROLLBACK), CreateStmt, DropStmt,
//   AlterTableStmt (ADD/DROP COLUMN), RenameStmt (RENAME COLUMN),
//   IndexStmt, TruncateStmt, VariableSetStmt, CreateCommandTag.
//
// Statements are produced by the parser (raw_parser + parse_analyze) and
// then dispatched to ProcessUtility directly, exercising the full
// parse → utility dispatch path.

#include "pgcpp/protocol/utility.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "pgcpp/access/heapam.hpp"
#include "pgcpp/access/rel.hpp"
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
#include "pgcpp/parser/parsenodes.hpp"
#include "pgcpp/parser/parser.hpp"
#include "pgcpp/protocol/pqformat.hpp"
#include "pgcpp/storage/bufmgr.hpp"
#include "pgcpp/storage/smgr.hpp"
#include "pgcpp/transaction/transam.hpp"
#include "pgcpp/transaction/xact.hpp"
#include "pgcpp/types/datum.hpp"

using mytoydb::access::InitializeRelcache;
using mytoydb::access::ResetRelcache;
using mytoydb::catalog::BootstrapCatalog;
using mytoydb::catalog::Catalog;
using mytoydb::catalog::GetCatalog;
using mytoydb::catalog::SetCatalog;
using mytoydb::catalog::SetSysCache;
using mytoydb::catalog::SysCache;
using mytoydb::memory::AllocSetContext;
using mytoydb::parser::Node;
using mytoydb::parser::parse_analyze;
using mytoydb::parser::Query;
using mytoydb::parser::raw_parser;
using mytoydb::parser::RawStmt;
using mytoydb::protocol::CreateCommandTag;
using mytoydb::protocol::ProcessUtility;
using mytoydb::protocol::StringSink;
using mytoydb::storage::InitBufferPool;
using mytoydb::storage::SetStorageBaseDir;
using mytoydb::storage::ShutdownBufferPool;
using mytoydb::storage::smgrcloseall;
using mytoydb::transaction::BeginTransactionBlock;
using mytoydb::transaction::EndTransactionBlock;
using mytoydb::transaction::InitializeSnapshotManager;
using mytoydb::transaction::InitializeTransactionSystem;
using mytoydb::transaction::IsTransactionBlock;
using mytoydb::transaction::ResetTransactionState;

namespace {

class UtilityTest : public ::testing::Test {
protected:
    void SetUp() override {
        mytoydb::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("utility_test_context");
        mytoydb::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        BootstrapCatalog(catalog_);
        syscache_ = new SysCache();
        SetSysCache(syscache_);

        ResetTransactionState();
        InitializeTransactionSystem();
        InitializeSnapshotManager();
        BeginTransactionBlock();

        test_dir_ = "/tmp/mytoydb_utility_test_" + std::to_string(getpid());
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

        mytoydb::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    // Parse a SQL string and return the utility_stmt of the first Query.
    // Returns nullptr if the statement is not a utility statement.
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

    static void RunShell(const std::string& cmd) { std::system(cmd.c_str()); }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
    SysCache* syscache_ = nullptr;
    std::string test_dir_;
    StringSink sink_;
};

// --- TransactionStmt ---

TEST_F(UtilityTest, BeginReturnsBeginTag) {
    EXPECT_EQ(RunUtility("BEGIN;"), "BEGIN");
    EXPECT_TRUE(IsTransactionBlock());
}

TEST_F(UtilityTest, CommitReturnsCommitTag) {
    RunUtility("BEGIN;");
    EXPECT_EQ(RunUtility("COMMIT;"), "COMMIT");
    EXPECT_FALSE(IsTransactionBlock());
}

TEST_F(UtilityTest, RollbackReturnsRollbackTag) {
    RunUtility("BEGIN;");
    EXPECT_EQ(RunUtility("ROLLBACK;"), "ROLLBACK");
    EXPECT_FALSE(IsTransactionBlock());
}

// --- CreateStmt ---

TEST_F(UtilityTest, CreateTableReturnsTagAndCreatesCatalogEntry) {
    EXPECT_EQ(RunUtility("CREATE TABLE t1 (a int4, b int4);"), "CREATE TABLE");
    auto* cat = GetCatalog();
    ASSERT_NE(cat, nullptr);
    EXPECT_NE(cat->GetClassByName("t1"), nullptr);
}

TEST_F(UtilityTest, CreateTableIfNotExistsIsIdempotent) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunUtility("CREATE TABLE IF NOT EXISTS t1 (a int4);"), "CREATE TABLE");
}

// --- DropStmt ---

TEST_F(UtilityTest, DropTableReturnsTagAndRemovesCatalogEntry) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunUtility("DROP TABLE t1;"), "DROP TABLE");
    auto* cat = GetCatalog();
    ASSERT_NE(cat, nullptr);
    EXPECT_EQ(cat->GetClassByName("t1"), nullptr);
}

TEST_F(UtilityTest, DropTableIfExistsSkipsMissing) {
    EXPECT_EQ(RunUtility("DROP TABLE IF EXISTS nonexistent;"), "DROP TABLE");
}

// --- AlterTableStmt ---

TEST_F(UtilityTest, AlterTableAddColumnIncrementsNatts) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunUtility("ALTER TABLE t1 ADD COLUMN b int4;"), "ALTER TABLE");
    auto* cat = GetCatalog();
    auto* class_row = cat->GetClassByName("t1");
    ASSERT_NE(class_row, nullptr);
    EXPECT_EQ(class_row->relnatts, 2);
    auto attrs = cat->GetAttributes(class_row->oid);
    bool found_b = false;
    for (const auto& attr : attrs) {
        if (attr->attname == "b")
            found_b = true;
    }
    EXPECT_TRUE(found_b);
}

TEST_F(UtilityTest, AlterTableDropColumnMarksDropped) {
    RunUtility("CREATE TABLE t1 (a int4, b int4);");
    EXPECT_EQ(RunUtility("ALTER TABLE t1 DROP COLUMN b;"), "ALTER TABLE");
    auto* cat = GetCatalog();
    auto* class_row = cat->GetClassByName("t1");
    ASSERT_NE(class_row, nullptr);
    EXPECT_EQ(class_row->relnatts, 1);
    auto attrs = cat->GetAttributes(class_row->oid);
    for (const auto& attr : attrs) {
        if (attr->attname == "b") {
            EXPECT_TRUE(attr->attisdropped);
        }
    }
}

// --- RenameStmt ---

TEST_F(UtilityTest, RenameColumnChangesAttname) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunUtility("ALTER TABLE t1 RENAME COLUMN a TO x;"), "ALTER TABLE");
    auto* cat = GetCatalog();
    auto* class_row = cat->GetClassByName("t1");
    ASSERT_NE(class_row, nullptr);
    auto attrs = cat->GetAttributes(class_row->oid);
    bool found_x = false;
    for (const auto& attr : attrs) {
        if (attr->attname == "x")
            found_x = true;
        EXPECT_NE(attr->attname, "a");
    }
    EXPECT_TRUE(found_x);
}

TEST_F(UtilityTest, RenameTableChangesRelname) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunUtility("ALTER TABLE t1 RENAME TO t2;"), "ALTER TABLE");
    auto* cat = GetCatalog();
    EXPECT_EQ(cat->GetClassByName("t1"), nullptr);
    EXPECT_NE(cat->GetClassByName("t2"), nullptr);
}

// --- IndexStmt ---

TEST_F(UtilityTest, CreateIndexReturnsTagAndCreatesIndexEntry) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunUtility("CREATE INDEX idx1 ON t1 (a);"), "CREATE INDEX");
    auto* cat = GetCatalog();
    ASSERT_NE(cat, nullptr);
    EXPECT_NE(cat->GetClassByName("idx1"), nullptr);
}

// --- TruncateStmt ---

TEST_F(UtilityTest, TruncateTableReturnsTag) {
    RunUtility("CREATE TABLE t1 (a int4);");
    EXPECT_EQ(RunUtility("TRUNCATE TABLE t1;"), "TRUNCATE TABLE");
}

// --- VariableSetStmt ---

TEST_F(UtilityTest, SetReturnsSetTag) {
    EXPECT_EQ(RunUtility("SET search_path TO public;"), "SET");
}

TEST_F(UtilityTest, ResetReturnsResetTag) {
    EXPECT_EQ(RunUtility("RESET search_path;"), "RESET");
}

// --- VacuumStmt ---

TEST_F(UtilityTest, VacuumReturnsVacuumTag) {
    EXPECT_EQ(RunUtility("VACUUM;"), "VACUUM");
}

TEST_F(UtilityTest, AnalyzeReturnsAnalyzeTag) {
    EXPECT_EQ(RunUtility("ANALYZE;"), "ANALYZE");
}

// --- CreateCommandTag ---

TEST_F(UtilityTest, CreateCommandTag_Begin) {
    Node* stmt = ParseUtilityStmt("BEGIN;");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(CreateCommandTag(stmt), "BEGIN");
}

TEST_F(UtilityTest, CreateCommandTag_CreateTable) {
    Node* stmt = ParseUtilityStmt("CREATE TABLE t1 (a int4);");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(CreateCommandTag(stmt), "CREATE TABLE");
}

TEST_F(UtilityTest, CreateCommandTag_DropTable) {
    Node* stmt = ParseUtilityStmt("DROP TABLE t1;");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(CreateCommandTag(stmt), "DROP TABLE");
}

TEST_F(UtilityTest, CreateCommandTag_AlterTable) {
    Node* stmt = ParseUtilityStmt("ALTER TABLE t1 ADD COLUMN b int4;");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(CreateCommandTag(stmt), "ALTER TABLE");
}

TEST_F(UtilityTest, CreateCommandTag_RenameColumn) {
    Node* stmt = ParseUtilityStmt("ALTER TABLE t1 RENAME COLUMN a TO b;");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(CreateCommandTag(stmt), "ALTER TABLE");
}

TEST_F(UtilityTest, CreateCommandTag_CreateIndex) {
    Node* stmt = ParseUtilityStmt("CREATE INDEX idx ON t1 (a);");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(CreateCommandTag(stmt), "CREATE INDEX");
}

TEST_F(UtilityTest, CreateCommandTag_Truncate) {
    Node* stmt = ParseUtilityStmt("TRUNCATE TABLE t1;");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(CreateCommandTag(stmt), "TRUNCATE TABLE");
}

TEST_F(UtilityTest, CreateCommandTag_Vacuum) {
    Node* stmt = ParseUtilityStmt("VACUUM;");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(CreateCommandTag(stmt), "VACUUM");
}

TEST_F(UtilityTest, CreateCommandTag_Analyze) {
    Node* stmt = ParseUtilityStmt("ANALYZE;");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(CreateCommandTag(stmt), "ANALYZE");
}

TEST_F(UtilityTest, CreateCommandTag_Set) {
    Node* stmt = ParseUtilityStmt("SET search_path TO public;");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(CreateCommandTag(stmt), "SET");
}

TEST_F(UtilityTest, CreateCommandTag_Reset) {
    Node* stmt = ParseUtilityStmt("RESET search_path;");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(CreateCommandTag(stmt), "RESET");
}

// --- ProcessUtility null/unknown ---

TEST_F(UtilityTest, ProcessUtility_NullStmtReturnsEmpty) {
    EXPECT_EQ(ProcessUtility(nullptr, &sink_), "");
}

TEST_F(UtilityTest, CreateCommandTag_NullStmtReturnsEmpty) {
    EXPECT_EQ(CreateCommandTag(nullptr), "");
}

}  // namespace

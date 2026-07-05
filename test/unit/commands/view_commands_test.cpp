// view_commands_test.cpp — Focused unit tests for view DDL.
//
// Exercises CREATE / DROP VIEW through the full
// raw_parser → parse_analyze → ProcessUtility path, verifying both the
// returned command tag and the resulting catalog state.
//
// The fixture mirrors commands_test.cpp: it stands up the error, memory,
// catalog, syscache, transaction, storage, buffer-pool and relcache
// subsystems so utility statements run in a realistic environment.

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
using pgcpp::error::ErrorData;
using pgcpp::error::GetErrorData;
using pgcpp::memory::AllocSetContext;
using pgcpp::parser::Node;
using pgcpp::parser::parse_analyze;
using pgcpp::parser::Query;
using pgcpp::parser::raw_parser;
using pgcpp::parser::RawStmt;
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

class ViewCommandsTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("view_commands_test_context");
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

        test_dir_ = "/tmp/pgcpp_view_commands_test_" + std::to_string(getpid());
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

    // Run a utility statement that is expected to ereport(ERROR).
    // Returns true if an error was caught.
    bool RunUtilityExpectError(const std::string& sql) {
        bool caught = false;
        PG_TRY() {
            RunUtility(sql);
        }
        PG_CATCH() {
            caught = true;
            ErrorData* err = GetErrorData();
            EXPECT_NE(err, nullptr);
            EXPECT_TRUE(err->IsError());
        }
        PG_END_TRY();
        return caught;
    }

    // Return the pg_class row for `name`, or nullptr if absent.
    const FormData_pg_class* GetClassRow(const std::string& name) {
        return GetCatalog()->GetClassByName(name);
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

// --- CREATE VIEW ---

TEST_F(ViewCommandsTest, CreateViewBasic) {
    EXPECT_EQ(RunUtility("CREATE VIEW v1 AS SELECT 1 AS a;"), "CREATE VIEW");
    const FormData_pg_class* row = GetClassRow("v1");
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->relkind, RelKind::kView);
}

TEST_F(ViewCommandsTest, CreateViewIfNotExists) {
    // pgcpp's grammar only supports CREATE OR REPLACE VIEW, not
    // CREATE VIEW IF NOT EXISTS (ViewStmt has no if_not_exists field).
    // The statement is therefore a syntax error and must ereport(ERROR).
    EXPECT_TRUE(RunUtilityExpectError("CREATE VIEW IF NOT EXISTS v1 AS SELECT 1 AS a;"));
    EXPECT_EQ(GetClassRow("v1"), nullptr);
}

TEST_F(ViewCommandsTest, CreateViewAlreadyExists) {
    ASSERT_EQ(RunUtility("CREATE VIEW v1 AS SELECT 1 AS a;"), "CREATE VIEW");
    // A second CREATE without OR REPLACE must ereport(ERROR).
    EXPECT_TRUE(RunUtilityExpectError("CREATE VIEW v1 AS SELECT 1 AS a;"));
    // The original entry must still be present.
    EXPECT_NE(GetClassRow("v1"), nullptr);
}

TEST_F(ViewCommandsTest, CreateViewWithColumns) {
    // The column alias list (x, y) is parsed (ViewStmt.aliases) but not
    // persisted by pgcpp's DefineView stub; verify the command succeeds
    // and produces a view row.
    EXPECT_EQ(RunUtility("CREATE VIEW v1 (x, y) AS SELECT 1, 2;"), "CREATE VIEW");
    const FormData_pg_class* row = GetClassRow("v1");
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->relkind, RelKind::kView);
}

TEST_F(ViewCommandsTest, CreateOrReplaceView) {
    ASSERT_EQ(RunUtility("CREATE VIEW v1 AS SELECT 1 AS a;"), "CREATE VIEW");
    // CREATE OR REPLACE must drop the existing view and recreate it.
    EXPECT_EQ(RunUtility("CREATE OR REPLACE VIEW v1 AS SELECT 2 AS a;"), "CREATE VIEW");
    const FormData_pg_class* row = GetClassRow("v1");
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->relkind, RelKind::kView);
}

// --- DROP VIEW ---

TEST_F(ViewCommandsTest, DropView) {
    RunUtility("CREATE VIEW v1 AS SELECT 1 AS a;");
    ASSERT_NE(GetClassRow("v1"), nullptr);
    EXPECT_EQ(RunUtility("DROP VIEW v1;"), "DROP VIEW");
    EXPECT_EQ(GetClassRow("v1"), nullptr);
}

TEST_F(ViewCommandsTest, DropViewIfExists) {
    // DROP VIEW IF EXISTS on a non-existent view must not error.
    EXPECT_EQ(RunUtility("DROP VIEW IF EXISTS nonexistent;"), "DROP VIEW");
}

TEST_F(ViewCommandsTest, DropViewNotExist) {
    // DROP VIEW without IF EXISTS on a missing relation must ereport(ERROR).
    EXPECT_TRUE(RunUtilityExpectError("DROP VIEW nonexistent;"));
}

TEST_F(ViewCommandsTest, DropMultipleViews) {
    RunUtility("CREATE VIEW v1 AS SELECT 1 AS a;");
    RunUtility("CREATE VIEW v2 AS SELECT 1 AS a;");
    ASSERT_NE(GetClassRow("v1"), nullptr);
    ASSERT_NE(GetClassRow("v2"), nullptr);
    // A single DROP VIEW can list multiple objects (DropStmt.objects).
    EXPECT_EQ(RunUtility("DROP VIEW v1, v2;"), "DROP VIEW");
    EXPECT_EQ(GetClassRow("v1"), nullptr);
    EXPECT_EQ(GetClassRow("v2"), nullptr);
}

TEST_F(ViewCommandsTest, CreateViewFromTable) {
    ASSERT_EQ(RunUtility("CREATE TABLE t1 (a int);"), "CREATE TABLE");
    EXPECT_EQ(RunUtility("CREATE VIEW v1 AS SELECT a FROM t1;"), "CREATE VIEW");
    const FormData_pg_class* row = GetClassRow("v1");
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->relkind, RelKind::kView);
}

}  // namespace

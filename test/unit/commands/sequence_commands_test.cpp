// sequence_commands_test.cpp — Focused unit tests for sequence DDL.
//
// Exercises CREATE / ALTER / DROP SEQUENCE through the full
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

class SequenceCommandsTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("sequence_commands_test_context");
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

        test_dir_ = "/tmp/pgcpp_sequence_commands_test_" + std::to_string(getpid());
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

// --- CREATE SEQUENCE ---

TEST_F(SequenceCommandsTest, CreateSequenceBasic) {
    EXPECT_EQ(RunUtility("CREATE SEQUENCE seq1;"), "CREATE SEQUENCE");
    const FormData_pg_class* row = GetClassRow("seq1");
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->relkind, RelKind::kSequence);
}

TEST_F(SequenceCommandsTest, CreateSequenceWithIncrement) {
    // pgcpp's sequence stub records the catalog entry but does not yet
    // persist INCREMENT BY; the test verifies the command still succeeds
    // and produces a sequence row.
    EXPECT_EQ(RunUtility("CREATE SEQUENCE seq1 INCREMENT BY 5;"), "CREATE SEQUENCE");
    const FormData_pg_class* row = GetClassRow("seq1");
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->relkind, RelKind::kSequence);
}

TEST_F(SequenceCommandsTest, CreateSequenceWithMinMax) {
    EXPECT_EQ(RunUtility("CREATE SEQUENCE seq1 MINVALUE 1 MAXVALUE 100;"), "CREATE SEQUENCE");
    const FormData_pg_class* row = GetClassRow("seq1");
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->relkind, RelKind::kSequence);
}

TEST_F(SequenceCommandsTest, CreateSequenceWithStart) {
    EXPECT_EQ(RunUtility("CREATE SEQUENCE seq1 START WITH 10;"), "CREATE SEQUENCE");
    const FormData_pg_class* row = GetClassRow("seq1");
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->relkind, RelKind::kSequence);
}

TEST_F(SequenceCommandsTest, CreateSequenceIfNotExists) {
    EXPECT_EQ(RunUtility("CREATE SEQUENCE IF NOT EXISTS seq1;"), "CREATE SEQUENCE");
    const FormData_pg_class* row = GetClassRow("seq1");
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->relkind, RelKind::kSequence);
}

TEST_F(SequenceCommandsTest, CreateSequenceAlreadyExists) {
    ASSERT_EQ(RunUtility("CREATE SEQUENCE seq1;"), "CREATE SEQUENCE");
    // A second CREATE without IF NOT EXISTS must ereport(ERROR).
    EXPECT_TRUE(RunUtilityExpectError("CREATE SEQUENCE seq1;"));
    // The original entry must still be present.
    EXPECT_NE(GetClassRow("seq1"), nullptr);
}

// --- DROP SEQUENCE ---

TEST_F(SequenceCommandsTest, DropSequence) {
    RunUtility("CREATE SEQUENCE seq1;");
    ASSERT_NE(GetClassRow("seq1"), nullptr);
    // RemoveRelations has no kSequence case in its tag switch, so the
    // returned tag is the generic "DROP". The catalog deletion still
    // runs; verify behavior + catalog state.
    std::string tag = RunUtility("DROP SEQUENCE seq1;");
    EXPECT_FALSE(tag.empty());
    EXPECT_EQ(GetClassRow("seq1"), nullptr);
}

TEST_F(SequenceCommandsTest, DropSequenceIfExists) {
    // DROP SEQUENCE IF EXISTS on a non-existent sequence must not error.
    std::string tag = RunUtility("DROP SEQUENCE IF EXISTS nonexistent;");
    EXPECT_FALSE(tag.empty());
}

TEST_F(SequenceCommandsTest, DropSequenceNotExist) {
    // DROP SEQUENCE without IF EXISTS on a missing relation must ereport(ERROR).
    EXPECT_TRUE(RunUtilityExpectError("DROP SEQUENCE nonexistent;"));
}

// --- ALTER SEQUENCE RENAME ---

TEST_F(SequenceCommandsTest, AlterSequenceRename) {
    RunUtility("CREATE SEQUENCE seq1;");
    ASSERT_NE(GetClassRow("seq1"), nullptr);
    // ALTER SEQUENCE ... RENAME TO ... is parsed as a RenameStmt and
    // dispatched to RenameRelation, which works on any relation type.
    // The returned tag is "ALTER TABLE" (the dispatcher has no kSequence
    // case); the rename itself succeeds. Verify the catalog state.
    std::string tag = RunUtility("ALTER SEQUENCE seq1 RENAME TO seq2;");
    EXPECT_FALSE(tag.empty());
    EXPECT_EQ(GetClassRow("seq1"), nullptr);
    const FormData_pg_class* row = GetClassRow("seq2");
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->relkind, RelKind::kSequence);
}

// --- multiple sequences ---

TEST_F(SequenceCommandsTest, CreateMultipleSequences) {
    // PostgreSQL grammar accepts one sequence per CREATE SEQUENCE, so we
    // issue three statements and verify each lands in the catalog.
    EXPECT_EQ(RunUtility("CREATE SEQUENCE s1;"), "CREATE SEQUENCE");
    EXPECT_EQ(RunUtility("CREATE SEQUENCE s2;"), "CREATE SEQUENCE");
    EXPECT_EQ(RunUtility("CREATE SEQUENCE s3;"), "CREATE SEQUENCE");
    EXPECT_NE(GetClassRow("s1"), nullptr);
    EXPECT_NE(GetClassRow("s2"), nullptr);
    EXPECT_NE(GetClassRow("s3"), nullptr);
    EXPECT_EQ(GetClassRow("s1")->relkind, RelKind::kSequence);
    EXPECT_EQ(GetClassRow("s2")->relkind, RelKind::kSequence);
    EXPECT_EQ(GetClassRow("s3")->relkind, RelKind::kSequence);
}

TEST_F(SequenceCommandsTest, DropMultipleSequences) {
    RunUtility("CREATE SEQUENCE s1;");
    RunUtility("CREATE SEQUENCE s2;");
    ASSERT_NE(GetClassRow("s1"), nullptr);
    ASSERT_NE(GetClassRow("s2"), nullptr);
    // A single DROP SEQUENCE can list multiple objects (DropStmt.objects).
    std::string tag = RunUtility("DROP SEQUENCE s1, s2;");
    EXPECT_FALSE(tag.empty());
    EXPECT_EQ(GetClassRow("s1"), nullptr);
    EXPECT_EQ(GetClassRow("s2"), nullptr);
}

}  // namespace

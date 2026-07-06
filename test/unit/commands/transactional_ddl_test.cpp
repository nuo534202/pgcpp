// transactional_ddl_test.cpp — Unit tests for P1-2 DDL transactionalization.
//
// Verifies that DDL operations (CREATE TABLE, DROP TABLE) are properly
// transactional:
//   - ROLLBACK undoes DDL changes made in the transaction.
//   - COMMIT persists DDL changes.
//   - DROP TABLE inside a ROLLBACK'd transaction leaves the table intact.
//
// The test exercises the catalog snapshot mechanism: StartTransaction takes
// a deep-copy snapshot of all user-created catalog rows; AbortTransaction
// restores it; CommitTransaction discards it and persists via CommitDirty.

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <string>

#include "access/rel.hpp"
#include "catalog/bootstrap_catalog.hpp"
#include "catalog/catalog.hpp"
#include "catalog/syscache.hpp"
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
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::Oid;
using pgcpp::catalog::SetCatalog;
using pgcpp::catalog::SetSysCache;
using pgcpp::catalog::SysCache;
using pgcpp::memory::AllocSetContext;
using pgcpp::parser::parse_analyze;
using pgcpp::parser::raw_parser;
using pgcpp::parser::RawStmt;
using pgcpp::protocol::ProcessUtility;
using pgcpp::protocol::StringSink;
using pgcpp::storage::InitBufferPool;
using pgcpp::storage::SetStorageBaseDir;
using pgcpp::storage::ShutdownBufferPool;
using pgcpp::storage::smgrcloseall;
using pgcpp::transaction::AbortTransactionBlock;
using pgcpp::transaction::BeginTransactionBlock;
using pgcpp::transaction::EndTransactionBlock;
using pgcpp::transaction::InitializeSnapshotManager;
using pgcpp::transaction::InitializeTransactionSystem;
using pgcpp::transaction::ResetTransactionState;

namespace {

class TransactionalDdlTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("txn_ddl_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        BootstrapCatalog(catalog_);
        syscache_ = new SysCache();
        SetSysCache(syscache_);

        ResetTransactionState();
        InitializeTransactionSystem();
        InitializeSnapshotManager();

        test_dir_ = "/tmp/pgcpp_txn_ddl_test_" + std::to_string(getpid());
        SetStorageBaseDir(test_dir_);
        RunShell("rm -rf " + test_dir_);

        InitBufferPool(64);
        InitializeRelcache();
    }

    void TearDown() override {
        // Clean up any leftover transaction state.
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

    // Parse + analyze + execute a utility statement (DDL).
    std::string RunUtility(const std::string& sql) {
        std::vector<RawStmt*> raw = raw_parser(sql);
        if (raw.empty())
            return "";
        std::vector<pgcpp::parser::Query*> queries = parse_analyze(raw, sql.c_str());
        if (queries.empty())
            return "";
        return ProcessUtility(queries[0]->utility_stmt, &sink_);
    }

    // Check if a relation exists by name in the catalog.
    bool RelationExists(const std::string& name) {
        return GetCatalog()->GetClassByName(name) != nullptr;
    }

    // Helper to check if a function ereports an error.
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

// --- ROLLBACK tests ---

// BEGIN + CREATE TABLE + ROLLBACK: table should not exist after rollback.
TEST_F(TransactionalDdlTest, CreateTableRollbackUndoesCreation) {
    BeginTransactionBlock();
    RunUtility("CREATE TABLE foo (a INT4)");
    EXPECT_TRUE(RelationExists("foo"));
    AbortTransactionBlock();
    EXPECT_FALSE(RelationExists("foo"));
}

// BEGIN + CREATE TABLE + DROP TABLE + ROLLBACK: table should exist (DROP is undone).
TEST_F(TransactionalDdlTest, DropTableRollbackRestoresTable) {
    // First create and commit the table.
    BeginTransactionBlock();
    RunUtility("CREATE TABLE bar (a INT4)");
    EndTransactionBlock();
    InitializeSnapshotManager();
    EXPECT_TRUE(RelationExists("bar"));

    // Now drop it in a transaction that gets rolled back.
    BeginTransactionBlock();
    RunUtility("DROP TABLE bar");
    EXPECT_FALSE(RelationExists("bar"));
    AbortTransactionBlock();
    EXPECT_TRUE(RelationExists("bar"));
}

// --- COMMIT tests ---

// BEGIN + CREATE TABLE + COMMIT: table should exist after commit.
TEST_F(TransactionalDdlTest, CreateTableCommitPersistsTable) {
    BeginTransactionBlock();
    RunUtility("CREATE TABLE baz (a INT4)");
    EXPECT_TRUE(RelationExists("baz"));
    EndTransactionBlock();
    InitializeSnapshotManager();
    EXPECT_TRUE(RelationExists("baz"));
}

// BEGIN + CREATE TABLE + COMMIT + BEGIN + DROP TABLE + COMMIT: table should not exist.
TEST_F(TransactionalDdlTest, DropTableCommitPersistsDrop) {
    BeginTransactionBlock();
    RunUtility("CREATE TABLE qux (a INT4)");
    EndTransactionBlock();
    InitializeSnapshotManager();

    BeginTransactionBlock();
    RunUtility("DROP TABLE qux");
    EndTransactionBlock();
    InitializeSnapshotManager();
    EXPECT_FALSE(RelationExists("qux"));
}

// --- OID counter tests ---

// ROLLBACK does NOT restore the OID counter in the MVP (no WAL redo /
// pendingDeletes). A table created after rollback gets a new OID, leaving
// a gap. This is acceptable: the orphaned physical file from the rolled-back
// CREATE TABLE is never referenced again.
TEST_F(TransactionalDdlTest, RollbackLeavesOidGap) {
    BeginTransactionBlock();
    RunUtility("CREATE TABLE oid_test_1 (a INT4)");
    Oid oid1 = GetCatalog()->GetClassByName("oid_test_1")->oid;
    AbortTransactionBlock();
    EXPECT_FALSE(RelationExists("oid_test_1"));

    // After rollback, next_oid is NOT restored. A new table gets a different
    // (higher) OID, avoiding collision with the orphaned physical file.
    BeginTransactionBlock();
    RunUtility("CREATE TABLE oid_test_2 (a INT4)");
    Oid oid2 = GetCatalog()->GetClassByName("oid_test_2")->oid;
    EndTransactionBlock();
    InitializeSnapshotManager();

    EXPECT_NE(oid1, oid2);
    EXPECT_GT(oid2, oid1);
}

// --- Multiple DDL in one transaction ---

// BEGIN + CREATE TABLE + CREATE TABLE + ROLLBACK: neither table should exist.
TEST_F(TransactionalDdlTest, MultipleCreatesRollbackUndoesAll) {
    BeginTransactionBlock();
    RunUtility("CREATE TABLE multi_a (a INT4)");
    RunUtility("CREATE TABLE multi_b (a INT4)");
    EXPECT_TRUE(RelationExists("multi_a"));
    EXPECT_TRUE(RelationExists("multi_b"));
    AbortTransactionBlock();
    EXPECT_FALSE(RelationExists("multi_a"));
    EXPECT_FALSE(RelationExists("multi_b"));
}

// --- Catalog snapshot state ---

// Verify that the catalog snapshot is taken at StartTransaction and discarded
// at Commit/Abort.
TEST_F(TransactionalDdlTest, SnapshotLifecycle) {
    // No transaction active — no snapshot.
    EXPECT_FALSE(GetCatalog()->HasSnapshot());

    BeginTransactionBlock();
    // Transaction started — snapshot should exist.
    EXPECT_TRUE(GetCatalog()->HasSnapshot());

    AbortTransactionBlock();
    // After abort — snapshot discarded.
    EXPECT_FALSE(GetCatalog()->HasSnapshot());

    BeginTransactionBlock();
    EXPECT_TRUE(GetCatalog()->HasSnapshot());
    EndTransactionBlock();
    InitializeSnapshotManager();
    EXPECT_FALSE(GetCatalog()->HasSnapshot());
}

// --- Dirty flag ---

// A read-only transaction (no DDL) should not mark the catalog dirty.
TEST_F(TransactionalDdlTest, ReadOnlyTransactionNotDirty) {
    BeginTransactionBlock();
    EXPECT_FALSE(GetCatalog()->IsDirty());
    EndTransactionBlock();
    InitializeSnapshotManager();
}

// A DDL transaction should mark the catalog dirty.
TEST_F(TransactionalDdlTest, DdlTransactionMarksDirty) {
    BeginTransactionBlock();
    RunUtility("CREATE TABLE dirty_test (a INT4)");
    EXPECT_TRUE(GetCatalog()->IsDirty());
    EndTransactionBlock();
    InitializeSnapshotManager();
}

}  // namespace

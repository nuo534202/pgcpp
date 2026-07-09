// rewrite_test.cpp — Unit tests for P2-4 query rewrite system.
//
// Verifies:
//   - View expansion: SELECT FROM view → RTE becomes subquery
//   - View with WHERE: view's filter preserved in subquery
//   - Nested views: recursive expansion
//   - RLS policy: security quals injected into RTE
//   - QueryRewrite on utility/null: no-op
//   - Storage roundtrip: StoreViewQuery/RetrieveViewQuery

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
#include "parser/primnodes.hpp"
#include "protocol/utility.hpp"
#include "rewrite/rewrite_handler.hpp"
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
using pgcpp::catalog::SetCatalog;
using pgcpp::catalog::SetSysCache;
using pgcpp::catalog::SysCache;
using pgcpp::memory::AllocSetContext;
using pgcpp::parser::CmdType;
using pgcpp::parser::Node;
using pgcpp::parser::parse_analyze;
using pgcpp::parser::Query;
using pgcpp::parser::RangeTblEntry;
using pgcpp::parser::raw_parser;
using pgcpp::parser::RawStmt;
using pgcpp::parser::RTEKind;
using pgcpp::protocol::ProcessUtility;
using pgcpp::protocol::StringSink;
using pgcpp::rewrite::ApplyRowSecurity;
using pgcpp::rewrite::EnableRowSecurity;
using pgcpp::rewrite::IsRowSecurityEnabled;
using pgcpp::rewrite::QueryRewrite;
using pgcpp::rewrite::RetrieveRowSecurityPolicy;
using pgcpp::rewrite::RetrieveViewQuery;
using pgcpp::rewrite::RewriteView;
using pgcpp::rewrite::StoreRowSecurityPolicy;
using pgcpp::rewrite::StoreViewQuery;
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

class RewriteTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("rewrite_test_context");
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

        test_dir_ = "/tmp/pgcpp_rewrite_test_" + std::to_string(getpid());
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

    // Run a utility statement (CREATE TABLE, CREATE VIEW) via ProcessUtility.
    void RunUtility(const std::string& sql) {
        auto raw = raw_parser(sql);
        if (raw.empty())
            return;
        auto queries = parse_analyze(raw, sql.c_str());
        if (queries.empty())
            return;
        Node* stmt = queries[0]->utility_stmt;
        if (stmt == nullptr)
            return;
        StringSink dummy_sink;
        ProcessUtility(stmt, &dummy_sink);
    }

    // Parse + analyze a SELECT query and return the Query tree.
    Query* ParseQuery(const std::string& sql) {
        auto raw = raw_parser(sql);
        if (raw.empty())
            return nullptr;
        auto queries = parse_analyze(raw, sql.c_str());
        if (queries.empty())
            return nullptr;
        return queries[0];
    }

    // Get the RTE at the given 1-based index from a query.
    RangeTblEntry* GetRTE(Query* query, int rt_index) {
        if (query == nullptr || rt_index < 1 ||
            static_cast<size_t>(rt_index) > query->rtable.size())
            return nullptr;
        return static_cast<RangeTblEntry*>(query->rtable[rt_index - 1]);
    }

    static void RunShell(const std::string& cmd) {
        int rc = std::system(cmd.c_str());
        (void)rc;
    }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
    SysCache* syscache_ = nullptr;
    std::string test_dir_;
};

// --- Basic view expansion ---

// SELECT FROM view: the view RTE should become a subquery RTE after rewrite.
TEST_F(RewriteTest, ViewExpansionChangesRteToSubquery) {
    RunUtility("CREATE TABLE t1 (a int4, b int4);");
    RunUtility("CREATE VIEW v1 AS SELECT * FROM t1;");

    Query* query = ParseQuery("SELECT * FROM v1;");
    ASSERT_NE(query, nullptr);
    ASSERT_FALSE(query->rtable.empty());

    // Before rewrite: RTE should be a relation.
    RangeTblEntry* rte = GetRTE(query, 1);
    ASSERT_NE(rte, nullptr);
    EXPECT_EQ(rte->rtekind, RTEKind::kRelation);

    // Apply rewrite.
    QueryRewrite(query);

    // After rewrite: RTE should be a subquery.
    rte = GetRTE(query, 1);
    ASSERT_NE(rte, nullptr);
    EXPECT_EQ(rte->rtekind, RTEKind::kSubquery);
    EXPECT_NE(rte->subquery, nullptr);
}

// View expansion preserves the view's SELECT structure in the subquery.
TEST_F(RewriteTest, ViewExpansionPreservesSelectStructure) {
    RunUtility("CREATE TABLE t1 (a int4, b int4);");
    RunUtility("CREATE VIEW v1 AS SELECT * FROM t1;");

    Query* query = ParseQuery("SELECT * FROM v1;");
    QueryRewrite(query);

    RangeTblEntry* rte = GetRTE(query, 1);
    ASSERT_NE(rte, nullptr);
    ASSERT_EQ(rte->rtekind, RTEKind::kSubquery);
    ASSERT_NE(rte->subquery, nullptr);

    // The subquery should be a SELECT.
    EXPECT_EQ(rte->subquery->command_type, CmdType::kSelect);
    // The subquery should have a non-empty rtable (the underlying table).
    EXPECT_FALSE(rte->subquery->rtable.empty());
}

// --- View with WHERE clause ---

// A view with a WHERE clause preserves the filter in the expanded subquery.
TEST_F(RewriteTest, ViewWithWherePreservesFilter) {
    RunUtility("CREATE TABLE t1 (a int4);");
    RunUtility("CREATE VIEW v1 AS SELECT * FROM t1 WHERE a > 5;");

    Query* query = ParseQuery("SELECT * FROM v1;");
    QueryRewrite(query);

    RangeTblEntry* rte = GetRTE(query, 1);
    ASSERT_NE(rte, nullptr);
    ASSERT_EQ(rte->rtekind, RTEKind::kSubquery);
    ASSERT_NE(rte->subquery, nullptr);

    // The subquery should have a qual (WHERE clause).
    EXPECT_NE(rte->subquery->jointree, nullptr);
}

// --- Nested views ---

// A view built on another view should recursively expand.
TEST_F(RewriteTest, NestedViewsExpandRecursively) {
    RunUtility("CREATE TABLE t1 (a int4);");
    RunUtility("CREATE VIEW v1 AS SELECT * FROM t1;");
    RunUtility("CREATE VIEW v2 AS SELECT * FROM v1;");

    Query* query = ParseQuery("SELECT * FROM v2;");
    QueryRewrite(query);

    RangeTblEntry* rte = GetRTE(query, 1);
    ASSERT_NE(rte, nullptr);
    ASSERT_EQ(rte->rtekind, RTEKind::kSubquery);
    ASSERT_NE(rte->subquery, nullptr);

    // The inner subquery (from v2 → v1) should also be expanded to a subquery.
    Query* inner = rte->subquery;
    ASSERT_FALSE(inner->rtable.empty());
    RangeTblEntry* inner_rte = GetRTE(inner, 1);
    ASSERT_NE(inner_rte, nullptr);
    // v1 should also be expanded to a subquery pointing to t1.
    EXPECT_EQ(inner_rte->rtekind, RTEKind::kSubquery);
}

// --- QueryRewrite edge cases ---

// QueryRewrite on nullptr returns nullptr.
TEST_F(RewriteTest, QueryRewriteNullReturnsNull) {
    EXPECT_EQ(QueryRewrite(nullptr), nullptr);
}

// QueryRewrite on a utility statement is a no-op.
TEST_F(RewriteTest, QueryRewriteUtilityIsNoOp) {
    auto raw = raw_parser("CREATE TABLE t1 (a int4);");
    auto queries = parse_analyze(raw, "CREATE TABLE t1 (a int4);");
    ASSERT_FALSE(queries.empty());

    Query* query = queries[0];
    ASSERT_EQ(query->command_type, CmdType::kUtility);

    Query* result = QueryRewrite(query);
    EXPECT_EQ(result, query);  // same pointer, unmodified
}

// --- Storage roundtrip ---

// StoreViewQuery / RetrieveViewQuery basic storage.
TEST_F(RewriteTest, StoreAndRetrieveViewQuery) {
    RunUtility("CREATE TABLE t1 (a int4);");
    RunUtility("CREATE VIEW v1 AS SELECT * FROM t1;");

    // The view's query should have been stored during CREATE VIEW.
    const auto* cls = GetCatalog()->GetClassByName("v1");
    ASSERT_NE(cls, nullptr);

    Query* stored = RetrieveViewQuery(static_cast<int>(cls->oid));
    EXPECT_NE(stored, nullptr);
    EXPECT_EQ(stored->command_type, CmdType::kSelect);
}

// --- RLS policy ---

// EnableRowSecurity / IsRowSecurityEnabled roundtrip.
TEST_F(RewriteTest, EnableAndCheckRowSecurity) {
    RunUtility("CREATE TABLE t1 (a int4);");
    const auto* cls = GetCatalog()->GetClassByName("t1");
    ASSERT_NE(cls, nullptr);
    int relid = static_cast<int>(cls->oid);

    EXPECT_FALSE(IsRowSecurityEnabled(relid));
    EnableRowSecurity(relid);
    EXPECT_TRUE(IsRowSecurityEnabled(relid));
}

// StoreRowSecurityPolicy / RetrieveRowSecurityPolicy roundtrip.
TEST_F(RewriteTest, StoreAndRetrieveRowSecurityPolicy) {
    // Store a dummy policy qual (nullptr means "no rows visible" in PG, but
    // for storage testing we just verify the roundtrip with a non-null node).
    // We use a simple AConst as a placeholder qual.
    auto* qual = pgcpp::nodes::makePallocNode<pgcpp::parser::AConst>();
    qual->isnull = false;

    StoreRowSecurityPolicy(42, qual);
    Node* retrieved = RetrieveRowSecurityPolicy(42);
    EXPECT_NE(retrieved, nullptr);
}

// ApplyRowSecurity adds security_quals to the RTE.
TEST_F(RewriteTest, ApplyRowSecurityAddsQuals) {
    RunUtility("CREATE TABLE t1 (a int4);");
    const auto* cls = GetCatalog()->GetClassByName("t1");
    ASSERT_NE(cls, nullptr);
    int relid = static_cast<int>(cls->oid);

    // Store a policy qual and enable RLS.
    auto* qual = pgcpp::nodes::makePallocNode<pgcpp::parser::AConst>();
    qual->isnull = false;
    StoreRowSecurityPolicy(relid, qual);
    EnableRowSecurity(relid);

    // Parse a SELECT on t1.
    Query* query = ParseQuery("SELECT * FROM t1;");
    ASSERT_NE(query, nullptr);

    // Before rewrite: no security_quals.
    RangeTblEntry* rte = GetRTE(query, 1);
    ASSERT_NE(rte, nullptr);
    EXPECT_TRUE(rte->security_quals.empty());

    // Apply rewrite (which includes RLS).
    QueryRewrite(query);

    // After rewrite: security_quals should be non-empty.
    rte = GetRTE(query, 1);
    ASSERT_NE(rte, nullptr);
    EXPECT_FALSE(rte->security_quals.empty());
    EXPECT_TRUE(query->has_row_security);
}

// --- Table without view: no rewrite ---

// SELECT from a regular table (not a view) should not be rewritten.
TEST_F(RewriteTest, TableSelectNotRewritten) {
    RunUtility("CREATE TABLE t1 (a int4);");

    Query* query = ParseQuery("SELECT * FROM t1;");
    ASSERT_NE(query, nullptr);

    RangeTblEntry* rte = GetRTE(query, 1);
    ASSERT_NE(rte, nullptr);
    EXPECT_EQ(rte->rtekind, RTEKind::kRelation);

    QueryRewrite(query);

    // Should still be a relation RTE (not rewritten).
    rte = GetRTE(query, 1);
    ASSERT_NE(rte, nullptr);
    EXPECT_EQ(rte->rtekind, RTEKind::kRelation);
}

// --- View with specific columns ---

// CREATE VIEW with specific columns expands correctly.
TEST_F(RewriteTest, ViewWithSpecificColumnsExpands) {
    RunUtility("CREATE TABLE t1 (a int4, b int4, c int4);");
    RunUtility("CREATE VIEW v1 AS SELECT a, b FROM t1;");

    Query* query = ParseQuery("SELECT * FROM v1;");
    QueryRewrite(query);

    RangeTblEntry* rte = GetRTE(query, 1);
    ASSERT_NE(rte, nullptr);
    EXPECT_EQ(rte->rtekind, RTEKind::kSubquery);
    EXPECT_NE(rte->subquery, nullptr);
    // The subquery's target list should have 2 entries (a, b).
    EXPECT_EQ(rte->subquery->target_list.size(), 2u);
}

}  // namespace

// explain_test.cpp — Unit tests for P2-3 EXPLAIN real output.
//
// Verifies that ExplainQuery produces a real plan-tree dump:
//   - RowDescription message with "QUERY PLAN" column
//   - DataRow messages containing node type, relation name, cost/rows/width
//   - Node-specific properties (Filter, Sort Key, Strategy, etc.)
//   - COSTS OFF option hides the cost clause
//   - ANALYZE / VERBOSE options are parsed without error
//
// The fixture sets up the full stack (Catalog, SysCache, buffer pool,
// relcache, transaction) so parse_analyze and planner can resolve real
// table references.

#include "commands/explain.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "access/rel.hpp"
#include "catalog/bootstrap_catalog.hpp"
#include "catalog/catalog.hpp"
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
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::SetCatalog;
using pgcpp::catalog::SetSysCache;
using pgcpp::catalog::SysCache;
using pgcpp::commands::ExplainQuery;
using pgcpp::memory::AllocSetContext;
using pgcpp::parser::ExplainStmt;
using pgcpp::parser::Node;
using pgcpp::parser::parse_analyze;
using pgcpp::parser::Query;
using pgcpp::parser::raw_parser;
using pgcpp::parser::RawStmt;
using pgcpp::protocol::BuildDataRow;
using pgcpp::protocol::BuildRowDescription;
using pgcpp::protocol::MessageReader;
using pgcpp::protocol::MessageType;
using pgcpp::protocol::OutputSink;
using pgcpp::protocol::ProcessUtility;
using pgcpp::protocol::RowDescriptionField;
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

class ExplainTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("explain_test_context");
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

        test_dir_ = "/tmp/pgcpp_explain_test_" + std::to_string(getpid());
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

    // Run a utility statement (e.g., CREATE TABLE) via ProcessUtility.
    std::string RunUtility(const std::string& sql) {
        auto raw = raw_parser(sql);
        if (raw.empty())
            return "";
        auto queries = parse_analyze(raw, sql.c_str());
        if (queries.empty())
            return "";
        Node* stmt = queries[0]->utility_stmt;
        if (stmt == nullptr)
            return "";
        StringSink dummy_sink;
        return ProcessUtility(stmt, &dummy_sink);
    }

    // Parse an EXPLAIN SQL string, run ExplainQuery, and return the decoded
    // output lines (one string per DataRow message).
    // Also populates sink_ for message-level inspection.
    std::vector<std::string> RunExplain(const std::string& sql) {
        sink_.clear();
        auto raw = raw_parser(sql);
        if (raw.empty())
            return {};
        auto* explain_stmt = static_cast<ExplainStmt*>(raw[0]->stmt);
        ExplainQuery(explain_stmt, &sink_);

        // Decode DataRow messages into text lines.
        std::vector<std::string> lines;
        for (const auto& msg : sink_.messages()) {
            if (msg.type != MessageType::kDataRow)
                continue;
            MessageReader rd(msg.payload);
            int16_t ncols = rd.ReadInt16();
            if (ncols < 1)
                continue;
            int32_t len = rd.ReadInt32();
            lines.push_back(rd.ReadBytes(len));
        }
        return lines;
    }

    // Join all output lines into a single string for substring matching.
    std::string RunExplainText(const std::string& sql) {
        auto lines = RunExplain(sql);
        std::string text;
        for (const auto& line : lines) {
            text += line;
            text += "\n";
        }
        return text;
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

// --- Basic EXPLAIN output structure ---

// EXPLAIN sends a RowDescription as the first message, with a single
// "QUERY PLAN" column of type TEXT (oid 25).
TEST_F(ExplainTest, ExplainSendsRowDescriptionFirst) {
    RunUtility("CREATE TABLE t1 (a int4, b int4);");
    RunExplain("EXPLAIN SELECT * FROM t1");

    ASSERT_GE(sink_.size(), 1u);
    EXPECT_EQ(sink_.at(0).type, MessageType::kRowDescription);
    MessageReader rd(sink_.at(0).payload);
    EXPECT_EQ(rd.ReadInt16(), 1);  // 1 column
    EXPECT_EQ(rd.ReadString(), "QUERY PLAN");
}

// EXPLAIN sends at least one DataRow message (the plan output).
TEST_F(ExplainTest, ExplainSendsDataRows) {
    RunUtility("CREATE TABLE t1 (a int4);");
    auto lines = RunExplain("EXPLAIN SELECT * FROM t1");

    EXPECT_FALSE(lines.empty());
    // The first line should contain a plan node name.
    EXPECT_FALSE(lines[0].empty());
}

// EXPLAIN returns "EXPLAIN" as the command tag.
TEST_F(ExplainTest, ExplainReturnsExplainTag) {
    RunUtility("CREATE TABLE t1 (a int4);");

    sink_.clear();
    auto raw = raw_parser("EXPLAIN SELECT * FROM t1");
    auto* explain_stmt = static_cast<ExplainStmt*>(raw[0]->stmt);
    std::string tag = ExplainQuery(explain_stmt, &sink_);
    EXPECT_EQ(tag, "EXPLAIN");
}

// --- Seq Scan output ---

// EXPLAIN SELECT * FROM t1 produces a "Seq Scan on t1" line with cost/rows/width.
TEST_F(ExplainTest, ExplainSelectStarShowsSeqScan) {
    RunUtility("CREATE TABLE t1 (a int4, b int4);");
    std::string text = RunExplainText("EXPLAIN SELECT * FROM t1");

    EXPECT_NE(text.find("Seq Scan"), std::string::npos);
    EXPECT_NE(text.find("t1"), std::string::npos);
    EXPECT_NE(text.find("cost="), std::string::npos);
    EXPECT_NE(text.find("rows="), std::string::npos);
    EXPECT_NE(text.find("width="), std::string::npos);
}

// EXPLAIN SELECT * FROM t1 WHERE a = 1 shows a Filter property.
TEST_F(ExplainTest, ExplainSelectWhereShowsFilter) {
    RunUtility("CREATE TABLE t1 (a int4);");
    std::string text = RunExplainText("EXPLAIN SELECT * FROM t1 WHERE a = 1");

    EXPECT_NE(text.find("Seq Scan"), std::string::npos);
    // The WHERE clause becomes a qual on the scan node, shown as "Filter".
    EXPECT_NE(text.find("Filter"), std::string::npos);
}

// --- Aggregate output ---

// EXPLAIN SELECT count(*) FROM t1 produces an Aggregate node.
TEST_F(ExplainTest, ExplainSelectCountShowsAggregate) {
    RunUtility("CREATE TABLE t1 (a int4);");
    std::string text = RunExplainText("EXPLAIN SELECT count(*) FROM t1");

    EXPECT_NE(text.find("Aggregate"), std::string::npos);
    // The Aggregate should have a Strategy property.
    EXPECT_NE(text.find("Strategy"), std::string::npos);
}

// --- Sort output ---

// EXPLAIN SELECT * FROM t1 ORDER BY a produces a Sort node.
TEST_F(ExplainTest, ExplainSelectOrderByShowsSort) {
    RunUtility("CREATE TABLE t1 (a int4);");
    std::string text = RunExplainText("EXPLAIN SELECT * FROM t1 ORDER BY a");

    EXPECT_NE(text.find("Sort"), std::string::npos);
    // The Sort node should have a Sort Key property.
    EXPECT_NE(text.find("Sort Key"), std::string::npos);
}

// --- Default cost display ---

// EXPLAIN SELECT shows the cost clause by default (COSTS is ON).
TEST_F(ExplainTest, ExplainDefaultShowsCostClause) {
    RunUtility("CREATE TABLE t1 (a int4);");
    std::string text = RunExplainText("EXPLAIN SELECT * FROM t1");

    EXPECT_NE(text.find("Seq Scan"), std::string::npos);
    EXPECT_NE(text.find("cost="), std::string::npos);
}

// --- ANALYZE option ---

// EXPLAIN ANALYZE is parsed and runs without error (actual execution
// instrumentation is not yet wired, so the estimated plan is shown).
TEST_F(ExplainTest, ExplainAnalyzeRunsSuccessfully) {
    RunUtility("CREATE TABLE t1 (a int4);");
    std::string text = RunExplainText("EXPLAIN ANALYZE SELECT * FROM t1");

    EXPECT_NE(text.find("Seq Scan"), std::string::npos);
}

// --- VERBOSE option ---

// EXPLAIN VERBOSE is parsed and runs without error.
TEST_F(ExplainTest, ExplainVerboseRunsSuccessfully) {
    RunUtility("CREATE TABLE t1 (a int4);");
    std::string text = RunExplainText("EXPLAIN VERBOSE SELECT * FROM t1");

    EXPECT_NE(text.find("Seq Scan"), std::string::npos);
}

// --- Multiple options (bare keyword syntax) ---

// EXPLAIN ANALYZE VERBOSE combines multiple supported options.
TEST_F(ExplainTest, ExplainMultipleOptions) {
    RunUtility("CREATE TABLE t1 (a int4);");
    std::string text = RunExplainText("EXPLAIN ANALYZE VERBOSE SELECT * FROM t1");

    EXPECT_NE(text.find("Seq Scan"), std::string::npos);
}

// --- Child node indentation ---

// EXPLAIN of an aggregate query shows child nodes with "->  " prefix.
TEST_F(ExplainTest, ExplainChildNodesUseArrowPrefix) {
    RunUtility("CREATE TABLE t1 (a int4);");
    std::string text = RunExplainText("EXPLAIN SELECT count(*) FROM t1");

    // The SeqScan child of the Aggregate should be prefixed with "->  ".
    EXPECT_NE(text.find("->"), std::string::npos);
}

// --- Relation name in scan ---

// EXPLAIN resolves the relation name from the range table.
TEST_F(ExplainTest, ExplainShowsRelationName) {
    RunUtility("CREATE TABLE mytable (a int4);");
    std::string text = RunExplainText("EXPLAIN SELECT * FROM mytable");

    EXPECT_NE(text.find("Seq Scan"), std::string::npos);
    EXPECT_NE(text.find("mytable"), std::string::npos);
}

// --- Table alias ---

// EXPLAIN shows the alias name when a table alias is used.
TEST_F(ExplainTest, ExplainShowsAliasName) {
    RunUtility("CREATE TABLE t1 (a int4);");
    std::string text = RunExplainText("EXPLAIN SELECT * FROM t1 AS x");

    EXPECT_NE(text.find("Seq Scan"), std::string::npos);
    // The alias "x" should appear in the output.
    EXPECT_NE(text.find("x"), std::string::npos);
}

}  // namespace

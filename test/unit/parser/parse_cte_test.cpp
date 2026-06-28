// parse_cte_test.cpp — Unit tests for parse_cte (M5 Task 15.11.2).
//
// Tests transformWithClause, the WITH-clause / CTE parse-analysis entry
// point converted from PostgreSQL 15's src/backend/parser/parse_cte.c.
//
// After transformWithClause runs:
//   * Each CommonTableExpr's ctequery is replaced by an analyzed Query tree.
//   * Each CTE is added to pstate->p_ctenamespace so that downstream FROM
//     resolution can locate it by name.
//   * End-to-end: "WITH cte AS (SELECT ...) SELECT ... FROM cte" must
//     produce a Query whose range table contains a subquery RTE for cte.

#include "pgcpp/parser/parse_cte.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "pgcpp/catalog/bootstrap_catalog.hpp"
#include "pgcpp/catalog/catalog.hpp"
#include "pgcpp/catalog/syscache.hpp"
#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/alloc_set.hpp"
#include "pgcpp/common/memory/memory_context.hpp"
#include "pgcpp/parser/analyze.hpp"
#include "pgcpp/parser/parse_node.hpp"
#include "pgcpp/parser/parsenodes.hpp"
#include "pgcpp/parser/parser.hpp"
#include "pgcpp/parser/primnodes.hpp"

using mytoydb::catalog::BootstrapCatalog;
using mytoydb::catalog::Catalog;
using mytoydb::catalog::GetCatalog;
using mytoydb::catalog::GetSysCache;
using mytoydb::catalog::SetCatalog;
using mytoydb::catalog::SetSysCache;
using mytoydb::catalog::SysCache;
using mytoydb::memory::AllocSetContext;
using mytoydb::nodes::Node;
using mytoydb::nodes::NodeTag;
using mytoydb::nodes::nodeTag;
using mytoydb::parser::CommonTableExpr;
using mytoydb::parser::make_parsestate;
using mytoydb::parser::parse_analyze;
using mytoydb::parser::ParseState;
using mytoydb::parser::Query;
using mytoydb::parser::RangeTblEntry;
using mytoydb::parser::RangeVar;
using mytoydb::parser::raw_parser;
using mytoydb::parser::RawStmt;
using mytoydb::parser::RTEKind;
using mytoydb::parser::SelectStmt;
using mytoydb::parser::transformWithClause;
using mytoydb::parser::WithClause;

namespace {

class ParseCteTest : public ::testing::Test {
protected:
    void SetUp() override {
        mytoydb::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("parse_cte_test_context");
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

    // Parse a single statement (no analysis).
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

// Find the CTE named `name` in pstate->p_ctenamespace; else nullptr.
CommonTableExpr* FindCte(ParseState* pstate, const std::string& name) {
    for (Node* n : pstate->p_ctenamespace) {
        if (n == nullptr || n->GetTag() != NodeTag::kCommonTableExpr)
            continue;
        auto* cte = static_cast<CommonTableExpr*>(n);
        if (cte->ctename == name)
            return cte;
    }
    return nullptr;
}

// Count subquery RTEs in a range table.
int CountSubqueryRtes(const std::vector<Node*>& rtable) {
    int n = 0;
    for (Node* n_node : rtable) {
        if (n_node == nullptr || n_node->GetTag() != NodeTag::kRangeTblEntry)
            continue;
        auto* rte = static_cast<RangeTblEntry*>(n_node);
        if (rte->rtekind == RTEKind::kSubquery)
            ++n;
    }
    return n;
}

}  // namespace

// ===========================================================================
// transformWithClause — direct API tests
// ===========================================================================

TEST_F(ParseCteTest, TransformWithClauseAnalyzesCteQuery) {
    // WITH cte AS (SELECT 1) SELECT 1
    Node* stmt = ParseSingle("WITH cte AS (SELECT 1) SELECT 1");
    ASSERT_NE(stmt, nullptr);
    ASSERT_EQ(nodeTag(stmt), NodeTag::kSelectStmt);
    auto* sel = static_cast<SelectStmt*>(stmt);
    ASSERT_NE(sel->with_clause, nullptr);

    ParseState* pstate = make_parsestate(nullptr);
    transformWithClause(pstate, sel->with_clause);

    // After transform, the CTE should be in p_ctenamespace.
    CommonTableExpr* cte = FindCte(pstate, "cte");
    ASSERT_NE(cte, nullptr);

    // The CTE's ctequery should now be an analyzed Query (kQuery tag).
    ASSERT_NE(cte->ctequery, nullptr);
    EXPECT_EQ(cte->ctequery->GetTag(), NodeTag::kQuery);

    auto* q = static_cast<Query*>(cte->ctequery);
    EXPECT_EQ(q->command_type, mytoydb::parser::CmdType::kSelect);

    free_parsestate(pstate);
}

TEST_F(ParseCteTest, TransformWithClauseAddsMultipleCtesToNamespace) {
    Node* stmt = ParseSingle("WITH a AS (SELECT 1), b AS (SELECT 2) SELECT 1");
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    ASSERT_NE(sel->with_clause, nullptr);
    ASSERT_EQ(sel->with_clause->ctes.size(), 2u);

    ParseState* pstate = make_parsestate(nullptr);
    transformWithClause(pstate, sel->with_clause);

    EXPECT_NE(FindCte(pstate, "a"), nullptr);
    EXPECT_NE(FindCte(pstate, "b"), nullptr);
    EXPECT_EQ(pstate->p_ctenamespace.size(), 2u);

    free_parsestate(pstate);
}

TEST_F(ParseCteTest, TransformWithClauseRecordsRecursiveFlag) {
    Node* stmt = ParseSingle("WITH RECURSIVE cte AS (SELECT 1) SELECT 1");
    ASSERT_NE(stmt, nullptr);
    auto* sel = static_cast<SelectStmt*>(stmt);
    ASSERT_NE(sel->with_clause, nullptr);
    EXPECT_TRUE(sel->with_clause->recursive);

    ParseState* pstate = make_parsestate(nullptr);
    transformWithClause(pstate, sel->with_clause);

    CommonTableExpr* cte = FindCte(pstate, "cte");
    ASSERT_NE(cte, nullptr);
    EXPECT_TRUE(cte->cterecursive);

    free_parsestate(pstate);
}

TEST_F(ParseCteTest, TransformWithClauseNullIsNoop) {
    ParseState* pstate = make_parsestate(nullptr);
    // Should not crash; should not modify pstate.
    transformWithClause(pstate, nullptr);
    EXPECT_TRUE(pstate->p_ctenamespace.empty());
    free_parsestate(pstate);
}

// ===========================================================================
// End-to-end — WITH ... SELECT FROM cte via parse_analyze
// ===========================================================================

TEST_F(ParseCteTest, EndToEndWithClauseSelectFromCte) {
    // WITH cte AS (SELECT 1 AS x) SELECT x FROM cte
    auto stmts = raw_parser("WITH cte AS (SELECT 1 AS x) SELECT x FROM cte");
    ASSERT_EQ(stmts.size(), 1u);

    auto queries = parse_analyze(stmts, nullptr);
    ASSERT_EQ(queries.size(), 1u);
    Query* q = queries[0];
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q->command_type, mytoydb::parser::CmdType::kSelect);

    // The FROM clause should have produced a subquery RTE for cte.
    EXPECT_GE(CountSubqueryRtes(q->rtable), 1)
        << "expected CTE to be referenced via a subquery RTE";

    // Target list should have one entry with resname "x".
    ASSERT_EQ(q->target_list.size(), 1u);
    auto* te = static_cast<mytoydb::parser::TargetEntry*>(q->target_list[0]);
    EXPECT_EQ(te->resname, "x");
}

TEST_F(ParseCteTest, EndToEndMultipleCtesCanReferenceEachOther) {
    // WITH a AS (SELECT 1 AS x), b AS (SELECT x FROM a) SELECT x FROM b
    auto stmts = raw_parser("WITH a AS (SELECT 1 AS x), b AS (SELECT x FROM a) SELECT x FROM b");
    ASSERT_EQ(stmts.size(), 1u);

    auto queries = parse_analyze(stmts, nullptr);
    ASSERT_EQ(queries.size(), 1u);
    Query* q = queries[0];
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q->command_type, mytoydb::parser::CmdType::kSelect);
    // b is referenced by the outer query; a is referenced by b. The outer
    // range table should contain at least one subquery RTE (for b).
    EXPECT_GE(CountSubqueryRtes(q->rtable), 1);
}

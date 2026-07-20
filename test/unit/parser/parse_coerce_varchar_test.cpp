// parse_coerce_varchar_test.cpp — Unit tests for VARCHAR typmod coercion.
//
// Verifies that unknown literals cast to VARCHAR(N) honor the typmod:
//   - 'hello world'::VARCHAR(5) silently truncates to 'hello' (explicit cast
//     uses varchar_typmod_coerce with is_explicit=true, matching PostgreSQL 15)
//   - 'hello'::VARCHAR(5) returns 'hello' (no truncation)
//   - 'hi'::VARCHAR(5) returns 'hi' (no padding — VARCHAR is variable-length)
//   - 'hello world'::VARCHAR returns 'hello world' (no typmod, no truncation)
//   - 'hello     '::VARCHAR(5) returns 'hello' (trailing spaces truncated)
//   - INSERT into a VARCHAR(5) column errors on non-space overflow
//     (assignment cast uses is_explicit=false → errors per varcharin())

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "catalog/bootstrap_catalog.hpp"
#include "catalog/catalog.hpp"
#include "catalog/pg_attribute.hpp"
#include "catalog/pg_class.hpp"
#include "catalog/pg_type.hpp"
#include "catalog/syscache.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "parser/analyze.hpp"
#include "parser/parse_coerce.hpp"
#include "parser/parse_node.hpp"
#include "parser/parse_type.hpp"
#include "parser/parsenodes.hpp"
#include "parser/parser.hpp"
#include "parser/primnodes.hpp"
#include "types/builtins.hpp"
#include "types/datum.hpp"

using pgcpp::catalog::BootstrapCatalog;
using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_attribute;
using pgcpp::catalog::FormData_pg_class;
using pgcpp::catalog::FormData_pg_type;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::GetSysCache;
using pgcpp::catalog::kInvalidOid;
using pgcpp::catalog::Oid;
using pgcpp::catalog::RelKind;
using pgcpp::catalog::RelPersistence;
using pgcpp::catalog::SetCatalog;
using pgcpp::catalog::SetSysCache;
using pgcpp::catalog::SysCache;
using pgcpp::memory::AllocSetContext;
using pgcpp::nodes::makePallocNode;
using pgcpp::nodes::Node;
using pgcpp::nodes::NodeTag;
using pgcpp::nodes::nodeTag;
using pgcpp::parser::CmdType;
using pgcpp::parser::Const;
using pgcpp::parser::parse_analyze;
using pgcpp::parser::Query;
using pgcpp::parser::RangeTblEntry;
using pgcpp::parser::raw_parser;
using pgcpp::parser::RawStmt;
using pgcpp::parser::RTEKind;
using pgcpp::parser::TargetEntry;
using pgcpp::types::kVarcharOid;
using pgcpp::types::TextDatumToString;
using pgcpp::types::varchar_in;
using pgcpp::types::varchar_out;

namespace {

// VARHDRSZ — PostgreSQL's varlena header size, used to encode typmods for
// varlena string types. Matches kVarHdrSz in src/types/builtins.cpp.
constexpr int32_t kVarHdrSz = 4;

// Test fixture providing a memory context, catalog, and a test table with
// a VARCHAR(5) column (atttypmod = VARHDRSZ + 5 = 9).
class ParseCoerceVarcharTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("parse_coerce_varchar_test");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        BootstrapCatalog(catalog_);

        syscache_ = new SysCache();
        SetSysCache(syscache_);

        SetupTestTable();
    }

    void TearDown() override {
        SetSysCache(nullptr);
        SetCatalog(nullptr);
        delete syscache_;
        delete catalog_;

        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    // Create a test table "vc_test" with a single VARCHAR(5) column "vc5".
    // typmod = VARHDRSZ + 5 = 9 (PostgreSQL encoding for varchar length).
    void SetupTestTable() {
        auto* class_row = makePallocNode<FormData_pg_class>();
        class_row->relname = "vc_test";
        class_row->oid = 17000;
        class_row->relkind = RelKind::kRelation;
        catalog_->InsertClass(class_row);

        AddAttribute(17000, "vc5", 1, kVarcharOid, kVarHdrSz + 5);
    }

    void AddAttribute(Oid relid, const std::string& name, int16_t attnum, Oid typid,
                      int32_t typmod) {
        auto* attr = makePallocNode<FormData_pg_attribute>();
        attr->attrelid = relid;
        attr->attname = name;
        attr->attnum = attnum;
        attr->atttypid = typid;
        attr->atttypmod = typmod;
        catalog_->InsertAttribute(attr);
    }

    // Helper: parse and analyze a SQL string, returning the first Query.
    // Takes const char* (not const std::string&) to avoid constructing a
    // temporary std::string that would leak if parse_analyze ereports(ERROR)
    // (longjmp bypasses the temporary's destructor).
    Query* AnalyzeSingle(const char* sql) {
        auto stmts = raw_parser(sql);
        if (stmts.empty())
            return nullptr;
        auto queries = parse_analyze(stmts, sql);
        if (queries.empty())
            return nullptr;
        return queries[0];
    }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
    SysCache* syscache_ = nullptr;
};

// Helper: check if a callable ereports(ERROR).
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

// Extract the constvalue of the first target-list Const as a std::string.
// Returns nullptr if the target list is empty or the first target is not
// a Const.
const Const* FirstTargetConst(const Query* qry) {
    if (qry == nullptr || qry->target_list.empty())
        return nullptr;
    Node* tle_node = qry->target_list[0];
    if (nodeTag(tle_node) != NodeTag::kTargetEntry)
        return nullptr;
    auto* tle = static_cast<TargetEntry*>(tle_node);
    if (tle->expr == nullptr || nodeTag(tle->expr) != NodeTag::kConst)
        return nullptr;
    return static_cast<Const*>(tle->expr);
}

}  // namespace

// ===========================================================================
// Explicit cast: <str>::VARCHAR(N)
// ===========================================================================

TEST_F(ParseCoerceVarcharTest, VarcharOfExactLengthIsNotTruncated) {
    // 'hello'::VARCHAR(5) — exactly 5 chars, no truncation needed.
    Query* qry = AnalyzeSingle("SELECT 'hello'::VARCHAR(5) AS v");
    ASSERT_NE(qry, nullptr);
    const Const* con = FirstTargetConst(qry);
    ASSERT_NE(con, nullptr);
    EXPECT_EQ(con->consttype, kVarcharOid);
    EXPECT_EQ(con->consttypmod, kVarHdrSz + 5);

    char* out = varchar_out(con->constvalue);
    EXPECT_STREQ(out, "hello");
}

TEST_F(ParseCoerceVarcharTest, ShortStringIsNotPaddedInVarchar) {
    // 'hi'::VARCHAR(5) — VARCHAR is variable-length, no padding (unlike CHAR).
    Query* qry = AnalyzeSingle("SELECT 'hi'::VARCHAR(5) AS v");
    ASSERT_NE(qry, nullptr);
    const Const* con = FirstTargetConst(qry);
    ASSERT_NE(con, nullptr);
    EXPECT_EQ(con->consttype, kVarcharOid);

    char* out = varchar_out(con->constvalue);
    EXPECT_STREQ(out, "hi");
}

TEST_F(ParseCoerceVarcharTest, VarcharWithoutTypmodDoesNotTruncate) {
    // 'hello world'::VARCHAR — no typmod, no truncation.
    Query* qry = AnalyzeSingle("SELECT 'hello world'::VARCHAR AS v");
    ASSERT_NE(qry, nullptr);
    const Const* con = FirstTargetConst(qry);
    ASSERT_NE(con, nullptr);
    EXPECT_EQ(con->consttype, kVarcharOid);
    // No typmod means -1, not the default 0.
    EXPECT_EQ(con->consttypmod, -1);

    char* out = varchar_out(con->constvalue);
    EXPECT_STREQ(out, "hello world");
}

TEST_F(ParseCoerceVarcharTest, VarcharTruncatesTrailingSpaces) {
    // 'hello     '::VARCHAR(5) — overflow is all spaces, truncate silently.
    Query* qry = AnalyzeSingle("SELECT 'hello     '::VARCHAR(5) AS v");
    ASSERT_NE(qry, nullptr);
    const Const* con = FirstTargetConst(qry);
    ASSERT_NE(con, nullptr);
    EXPECT_EQ(con->consttype, kVarcharOid);
    EXPECT_EQ(con->consttypmod, kVarHdrSz + 5);

    char* out = varchar_out(con->constvalue);
    EXPECT_STREQ(out, "hello");
}

TEST_F(ParseCoerceVarcharTest, VarcharExplicitCastSilentlyTruncatesNonSpaceOverflow) {
    // 'hello world'::VARCHAR(5) — explicit cast silently truncates to
    // 'hello' regardless of overflow content. PostgreSQL's cast machinery
    // routes through varchar_typmod_coerce() with is_explicit=true for the
    // ::VARCHAR(N) syntax (verified against PostgreSQL 15.15). The
    // non-space-overflow ERROR only fires for implicit/assignment casts
    // (INSERT, etc.), which use is_explicit=false.
    Query* qry = AnalyzeSingle("SELECT 'hello world'::VARCHAR(5) AS v");
    ASSERT_NE(qry, nullptr);
    const Const* con = FirstTargetConst(qry);
    ASSERT_NE(con, nullptr);
    EXPECT_EQ(con->consttype, kVarcharOid);
    EXPECT_EQ(con->consttypmod, kVarHdrSz + 5);

    char* out = varchar_out(con->constvalue);
    EXPECT_STREQ(out, "hello");
}

// ===========================================================================
// INSERT into VARCHAR(N) column — assignment cast (implicit)
// ===========================================================================

TEST_F(ParseCoerceVarcharTest, InsertShortStringIntoVarchar5Succeeds) {
    // INSERT INTO vc_test VALUES ('hi') — 'hi' fits, no truncation.
    Query* qry = AnalyzeSingle("INSERT INTO vc_test VALUES ('hi')");
    ASSERT_NE(qry, nullptr);
    EXPECT_EQ(qry->command_type, CmdType::kInsert);
    ASSERT_EQ(qry->rtable.size(), 1u);
    auto* rte = static_cast<RangeTblEntry*>(qry->rtable[0]);
    EXPECT_EQ(rte->rtekind, RTEKind::kRelation);

    // The single target entry's expr should be a Const of type varchar.
    ASSERT_EQ(qry->target_list.size(), 1u);
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    ASSERT_EQ(nodeTag(tle->expr), NodeTag::kConst);
    auto* con = static_cast<Const*>(tle->expr);
    EXPECT_EQ(con->consttype, kVarcharOid);
    // Assignment cast preserves the column typmod.
    EXPECT_EQ(con->consttypmod, kVarHdrSz + 5);

    char* out = varchar_out(con->constvalue);
    EXPECT_STREQ(out, "hi");
}

TEST_F(ParseCoerceVarcharTest, InsertExactLengthIntoVarchar5Succeeds) {
    // INSERT INTO vc_test VALUES ('hello') — exactly 5 chars, fits.
    Query* qry = AnalyzeSingle("INSERT INTO vc_test VALUES ('hello')");
    ASSERT_NE(qry, nullptr);
    ASSERT_EQ(qry->target_list.size(), 1u);
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    ASSERT_EQ(nodeTag(tle->expr), NodeTag::kConst);
    auto* con = static_cast<Const*>(tle->expr);
    EXPECT_EQ(con->consttype, kVarcharOid);

    char* out = varchar_out(con->constvalue);
    EXPECT_STREQ(out, "hello");
}

TEST_F(ParseCoerceVarcharTest, InsertNonSpaceOverflowIntoVarchar5Errors) {
    // INSERT INTO vc_test VALUES ('hello world') — overflow ' world' is
    // non-space, must error in assignment context too (PG semantics).
    auto stmts = raw_parser("INSERT INTO vc_test VALUES ('hello world')");
    ASSERT_FALSE(stmts.empty());

    bool raised = false;
    PG_TRY() {
        auto queries = parse_analyze(stmts, "INSERT INTO vc_test VALUES ('hello world')");
        (void)queries;
    }
    PG_CATCH() {
        raised = true;
    }
    PG_END_TRY();
    EXPECT_TRUE(raised);
}

TEST_F(ParseCoerceVarcharTest, InsertSpaceOverflowIntoVarchar5Truncates) {
    // INSERT INTO vc_test VALUES ('hello     ') — overflow is all spaces,
    // assignment cast truncates silently.
    Query* qry = AnalyzeSingle("INSERT INTO vc_test VALUES ('hello     ')");
    ASSERT_NE(qry, nullptr);
    ASSERT_EQ(qry->target_list.size(), 1u);
    auto* tle = static_cast<TargetEntry*>(qry->target_list[0]);
    ASSERT_EQ(nodeTag(tle->expr), NodeTag::kConst);
    auto* con = static_cast<Const*>(tle->expr);
    EXPECT_EQ(con->consttype, kVarcharOid);

    char* out = varchar_out(con->constvalue);
    EXPECT_STREQ(out, "hello");
}

// ===========================================================================
// Direct varchar_in / varchar_typmod_coerce sanity check (no SQL layer)
// ===========================================================================

TEST_F(ParseCoerceVarcharTest, VarcharInRespectsTypmod) {
    // Direct call to varchar_in: 'hello' fits, 'hello world' errors.
    auto d = varchar_in("hello", kVarHdrSz + 5);
    char* out = varchar_out(d);
    EXPECT_STREQ(out, "hello");

    EXPECT_TRUE(RaisesError([&] { varchar_in("hello world", kVarHdrSz + 5); }));
}

TEST_F(ParseCoerceVarcharTest, VarcharInWithoutTypmodDoesNotTruncate) {
    // typmod = -1 means no typmod, no truncation.
    auto d = varchar_in("hello world", -1);
    char* out = varchar_out(d);
    EXPECT_STREQ(out, "hello world");
}

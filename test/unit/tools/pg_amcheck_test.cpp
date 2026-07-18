// pg_amcheck_test.cpp — Unit tests for the pg_amcheck tool.
//
// Verifies the SQL builders, the boolean result interpretation, and the
// error paths of RunAmcheck (no server reachable, so most paths hit
// kConnectFailed). The SQL builders are the testable surface that does
// not require a live server.
#include "tools/pg_amcheck.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include <string>

namespace pt = pgcpp::tools;
using std::string;

TEST(PgAmcheckTest, BuildHeapCheckSqlBasic) {
    string sql = pt::BuildHeapCheckSql("accounts");
    EXPECT_EQ(sql, "SELECT * FROM verify_heapam('accounts'::regclass);");
}

TEST(PgAmcheckTest, BuildHeapCheckSqlEscapesQuotes) {
    string sql = pt::BuildHeapCheckSql("jane's table");
    EXPECT_EQ(sql, "SELECT * FROM verify_heapam('jane''s table'::regclass);");
}

TEST(PgAmcheckTest, BuildIndexCheckSqlBasic) {
    string sql = pt::BuildIndexCheckSql("accounts_pkey");
    EXPECT_EQ(sql, "SELECT bt_index_check('accounts_pkey'::regclass);");
}

TEST(PgAmcheckTest, BuildIndexParentCheckSqlBasic) {
    string sql = pt::BuildIndexParentCheckSql("accounts_pkey");
    EXPECT_EQ(sql, "SELECT bt_index_parent_check('accounts_pkey'::regclass);");
}

TEST(PgAmcheckTest, BuildListTablesSqlNoFilter) {
    string sql = pt::BuildAmcheckTableListSql("");
    EXPECT_NE(sql.find("pg_class"), string::npos);
    EXPECT_NE(sql.find("relkind = 'r'"), string::npos);
    EXPECT_NE(sql.find("nspname = 'public'"), string::npos);
    EXPECT_EQ(sql.find("LIKE"), string::npos);
    EXPECT_NE(sql.find("ORDER BY"), string::npos);
}

TEST(PgAmcheckTest, BuildListTablesSqlWithPattern) {
    string sql = pt::BuildAmcheckTableListSql("user");
    EXPECT_NE(sql.find("LIKE"), string::npos);
    EXPECT_NE(sql.find("%user%"), string::npos);
}

TEST(PgAmcheckTest, BuildListIndexesSqlNoFilter) {
    string sql = pt::BuildListIndexesSql("");
    EXPECT_NE(sql.find("pg_class"), string::npos);
    EXPECT_NE(sql.find("relkind = 'i'"), string::npos);
    EXPECT_NE(sql.find("nspname = 'public'"), string::npos);
    EXPECT_EQ(sql.find("LIKE"), string::npos);
}

TEST(PgAmcheckTest, BuildListIndexesSqlWithPattern) {
    string sql = pt::BuildListIndexesSql("pkey");
    EXPECT_NE(sql.find("%pkey%"), string::npos);
}

TEST(PgAmcheckTest, BuildListDatabasesSqlShape) {
    string sql = pt::BuildAmcheckDatabaseListSql();
    EXPECT_NE(sql.find("pg_database"), string::npos);
    EXPECT_NE(sql.find("datallowconn"), string::npos);
    EXPECT_NE(sql.find("datistemplate"), string::npos);
    EXPECT_NE(sql.find("ORDER BY 1"), string::npos);
}

TEST(PgAmcheckTest, BuildCreateExtensionSql) {
    string sql = pt::BuildCreateExtensionSql();
    EXPECT_EQ(sql, "CREATE EXTENSION IF NOT EXISTS amcheck;");
}

TEST(PgAmcheckTest, InterpretCheckResultTrueVariants) {
    EXPECT_TRUE(pt::InterpretCheckResult("t"));
    EXPECT_TRUE(pt::InterpretCheckResult("true"));
    EXPECT_TRUE(pt::InterpretCheckResult("T"));
    EXPECT_TRUE(pt::InterpretCheckResult("TRUE"));
}

TEST(PgAmcheckTest, InterpretCheckResultFalseVariants) {
    EXPECT_FALSE(pt::InterpretCheckResult(""));
    EXPECT_FALSE(pt::InterpretCheckResult("f"));
    EXPECT_FALSE(pt::InterpretCheckResult("false"));
    EXPECT_FALSE(pt::InterpretCheckResult("(no rows)"));
}

TEST(PgAmcheckTest, RunAmcheckConnectFailsNoDbName) {
    // No dbname configured, no all_db — RunAmcheck returns kConnectFailed
    // because libpq cannot connect to an empty dbname.
    pt::AmcheckOptions opts;
    opts.dbname = "nonexistent_db_for_amcheck_test";
    opts.host = "127.0.0.1";
    opts.port = 1;  // unusable port
    pt::AmcheckStats stats;
    std::ostringstream out;
    pt::AmcheckResult r = pt::RunAmcheck(opts, stats, &out);
    EXPECT_EQ(r, pt::AmcheckResult::kConnectFailed);
    EXPECT_EQ(stats.databases_checked, 0);
    EXPECT_EQ(stats.relations_checked, 0);
    EXPECT_EQ(stats.corrupt, 0);
}

TEST(PgAmcheckTest, RunAmcheckAllDbsConnectFails) {
    pt::AmcheckOptions opts;
    opts.all_db = true;
    opts.host = "127.0.0.1";
    opts.port = 1;  // unusable port
    pt::AmcheckStats stats;
    std::ostringstream out;
    pt::AmcheckResult r = pt::RunAmcheck(opts, stats, &out);
    EXPECT_EQ(r, pt::AmcheckResult::kConnectFailed);
}

TEST(PgAmcheckTest, AmcheckStatsAllOkEmpty) {
    pt::AmcheckStats s;
    EXPECT_TRUE(s.AllOk());
    s.corrupt = 1;
    EXPECT_FALSE(s.AllOk());
    s.corrupt = 0;
    s.errors = 1;
    EXPECT_FALSE(s.AllOk());
}

TEST(PgAmcheckTest, BuildHeapCheckSqlInjectsRegclassCast) {
    // The cast is required so a name like 'a-b' isn't interpreted as an
    // arithmetic expression.
    string sql = pt::BuildHeapCheckSql("a-b");
    EXPECT_NE(sql.find("::regclass"), string::npos);
}

TEST(PgAmcheckTest, BuildListTablesSqlOrdersByRelname) {
    string sql = pt::BuildAmcheckTableListSql("");
    EXPECT_NE(sql.find("ORDER BY c.relname"), string::npos);
}

TEST(PgAmcheckTest, BuildListIndexesSqlOrdersByRelname) {
    string sql = pt::BuildListIndexesSql("");
    EXPECT_NE(sql.find("ORDER BY c.relname"), string::npos);
}

TEST(PgAmcheckTest, QuoteLiteralDoublingViaBuildHeapCheckSql) {
    // Multiple embedded quotes must be doubled.
    string sql = pt::BuildHeapCheckSql("a''b");
    EXPECT_EQ(sql, "SELECT * FROM verify_heapam('a''''b'::regclass);");
}

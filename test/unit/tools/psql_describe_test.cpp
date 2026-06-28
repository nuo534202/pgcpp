// psql_describe_test.cpp — Unit tests for SQL builders backing psql
// backslash meta-commands (\d, \dt, \dv, \l, \du).
//
// These tests verify that the SQL strings generated for each meta-command
// are well-formed and contain the expected clauses. They do not require a
// running server — the SQL is sent to the server at runtime, but the
// generation logic is purely functional and can be unit-tested in isolation.
#include "pgcpp/tools/psql_describe.hpp"

#include <gtest/gtest.h>

#include <string>

using mytoydb::tools::BuildDescribeRelationSql;
using mytoydb::tools::BuildListDatabasesSql;
using mytoydb::tools::BuildListRolesSql;
using mytoydb::tools::BuildListTablesSql;
using mytoydb::tools::BuildListViewsSql;

namespace {

// Helper: case-insensitive substring test.
bool ContainsCi(const std::string& haystack, const std::string& needle) {
    if (needle.empty())
        return true;
    if (haystack.size() < needle.size())
        return false;
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (std::tolower(static_cast<unsigned char>(haystack[i + j])) !=
                std::tolower(static_cast<unsigned char>(needle[j]))) {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// \dt — list tables
// ---------------------------------------------------------------------------

TEST(PsqlDescribeTest, BuildListTablesSqlWithoutPattern) {
    std::string sql = BuildListTablesSql("");
    EXPECT_FALSE(sql.empty());
    EXPECT_TRUE(ContainsCi(sql, "pg_class"));
    EXPECT_TRUE(ContainsCi(sql, "relkind"));
    // PostgreSQL stores ordinary tables with relkind = 'r'.
    EXPECT_NE(sql.find("'r'"), std::string::npos);
    // Should filter out system schemas.
    EXPECT_TRUE(ContainsCi(sql, "pg_catalog"));
    // Should order by name.
    EXPECT_TRUE(ContainsCi(sql, "ORDER BY"));
    EXPECT_EQ(sql.back(), ';');
}

TEST(PsqlDescribeTest, BuildListTablesSqlWithPattern) {
    std::string sql = BuildListTablesSql("user_%");
    EXPECT_TRUE(ContainsCi(sql, "LIKE"));
    EXPECT_NE(sql.find("user_%"), std::string::npos);
}

TEST(PsqlDescribeTest, BuildListTablesSqlHasSchemaColumn) {
    std::string sql = BuildListTablesSql("");
    // \dt should expose the schema column (PostgreSQL convention).
    EXPECT_TRUE(ContainsCi(sql, "nspname") || ContainsCi(sql, "Schema"));
}

// ---------------------------------------------------------------------------
// \dv — list views
// ---------------------------------------------------------------------------

TEST(PsqlDescribeTest, BuildListViewsSqlUsesRelkindV) {
    std::string sql = BuildListViewsSql("");
    EXPECT_TRUE(ContainsCi(sql, "pg_class"));
    EXPECT_NE(sql.find("'v'"), std::string::npos);
}

TEST(PsqlDescribeTest, BuildListViewsSqlWithPattern) {
    std::string sql = BuildListViewsSql("v_%");
    EXPECT_TRUE(ContainsCi(sql, "LIKE"));
    EXPECT_NE(sql.find("v_%"), std::string::npos);
}

// ---------------------------------------------------------------------------
// \l — list databases
// ---------------------------------------------------------------------------

TEST(PsqlDescribeTest, BuildListDatabasesSql) {
    std::string sql = BuildListDatabasesSql();
    EXPECT_FALSE(sql.empty());
    EXPECT_TRUE(ContainsCi(sql, "pg_database") || ContainsCi(sql, "datname"));
    EXPECT_EQ(sql.back(), ';');
}

// ---------------------------------------------------------------------------
// \du — list roles
// ---------------------------------------------------------------------------

TEST(PsqlDescribeTest, BuildListRolesSql) {
    std::string sql = BuildListRolesSql();
    EXPECT_FALSE(sql.empty());
    // PostgreSQL exposes roles through pg_roles view.
    EXPECT_TRUE(ContainsCi(sql, "pg_roles") || ContainsCi(sql, "rolname"));
    EXPECT_EQ(sql.back(), ';');
}

// ---------------------------------------------------------------------------
// \d <name> — describe a relation
// ---------------------------------------------------------------------------

TEST(PsqlDescribeTest, BuildDescribeRelationSqlIncludesName) {
    std::string sql = BuildDescribeRelationSql("users");
    EXPECT_FALSE(sql.empty());
    EXPECT_NE(sql.find("users"), std::string::npos);
    EXPECT_TRUE(ContainsCi(sql, "pg_attribute"));
}

TEST(PsqlDescribeTest, BuildDescribeRelationSqlEndsWithSemicolon) {
    std::string sql = BuildDescribeRelationSql("orders");
    EXPECT_EQ(sql.back(), ';');
}

TEST(PsqlDescribeTest, BuildDescribeRelationSqlHasColumnAndType) {
    std::string sql = BuildDescribeRelationSql("orders");
    // \d should report column name and type.
    EXPECT_TRUE(ContainsCi(sql, "attname") || ContainsCi(sql, "Column"));
    EXPECT_TRUE(ContainsCi(sql, "atttypid") || ContainsCi(sql, "Type"));
}

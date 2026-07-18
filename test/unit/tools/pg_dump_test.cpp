// pg_dump_test.cpp — Unit tests for the pg_dump SQL statement builders.
//
// Covers:
//   - QuoteIdentifier / QuoteLiteral escaping (incl. embedded quotes).
//   - BuildDropTableStatement / BuildCreateTableStatement / BuildCopyHeader /
//     BuildInsertStatement (existing helpers, regression coverage).
//   - BuildCreateIndexStatement (UNIQUE & non-UNIQUE; multi-column).
//   - BuildDropIndexStatement.
//   - BuildCreateSequenceStatement (full options, partial options, cycle/no
//     cycle, no options at all).
//   - BuildDropSequenceStatement.
//   - BuildCreateViewStatement / BuildDropViewStatement.
//   - BuildGrantStatement (named role and PUBLIC fallback).
//
// DumpDatabase() itself requires a live server and is exercised by the SQL
// regression suite (test/regression/); it is not covered here.
#include "tools/pg_dump.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace pt = pgcpp::tools;

// ---------------------------------------------------------------------------
// Identifier / literal quoting.
// ---------------------------------------------------------------------------

TEST(PgDumpTest, QuoteIdentifierWrapsInDoubleQuotes) {
    EXPECT_EQ(pt::QuoteIdentifier("foo"), "\"foo\"");
}

TEST(PgDumpTest, QuoteIdentifierDoublesEmbeddedDoubleQuotes) {
    EXPECT_EQ(pt::QuoteIdentifier("a\"b"), "\"a\"\"b\"");
}

TEST(PgDumpTest, QuoteLiteralWrapsInSingleQuotes) {
    EXPECT_EQ(pt::QuoteLiteral("foo"), "'foo'");
}

TEST(PgDumpTest, QuoteLiteralDoublesEmbeddedSingleQuotes) {
    EXPECT_EQ(pt::QuoteLiteral("a'b"), "'a''b'");
}

// ---------------------------------------------------------------------------
// Table builders.
// ---------------------------------------------------------------------------

TEST(PgDumpTest, BuildDropTableStatement) {
    EXPECT_EQ(pt::BuildDropTableStatement("t"), "DROP TABLE IF EXISTS \"t\";\n");
}

TEST(PgDumpTest, BuildCreateTableStatement) {
    std::vector<std::pair<std::string, std::string>> cols = {
        {"id", "integer"},
        {"name", "text"},
    };
    const std::string expected =
        "CREATE TABLE \"t\" (\n"
        "  \"id\" integer,\n"
        "  \"name\" text\n"
        ");\n";
    EXPECT_EQ(pt::BuildCreateTableStatement("t", cols), expected);
}

TEST(PgDumpTest, BuildCreateTableStatementEmptyColumns) {
    const std::string expected = "CREATE TABLE \"t\" (\n);\n";
    EXPECT_EQ(pt::BuildCreateTableStatement("t", {}), expected);
}

TEST(PgDumpTest, BuildCopyHeader) {
    std::vector<std::string> cols = {"id", "name"};
    const std::string expected = "COPY \"t\" (\"id\", \"name\") FROM stdin;\n";
    EXPECT_EQ(pt::BuildCopyHeader("t", cols), expected);
}

TEST(PgDumpTest, BuildInsertStatement) {
    std::vector<std::string> cols = {"id", "name"};
    std::vector<std::string> vals = {"1", "alice"};
    const std::string expected = "INSERT INTO \"t\" (\"id\", \"name\") VALUES ('1', 'alice');\n";
    EXPECT_EQ(pt::BuildInsertStatement("t", cols, vals), expected);
}

TEST(PgDumpTest, BuildInsertStatementEmptyValues) {
    const std::string expected = "INSERT INTO \"t\" () VALUES ();\n";
    EXPECT_EQ(pt::BuildInsertStatement("t", {}, {}), expected);
}

// ---------------------------------------------------------------------------
// Index builders.
// ---------------------------------------------------------------------------

TEST(PgDumpTest, BuildCreateIndexStatementNonUnique) {
    std::vector<std::string> cols = {"a", "b"};
    const std::string expected = "CREATE INDEX \"idx\" ON \"t\" (\"a\", \"b\");\n";
    EXPECT_EQ(pt::BuildCreateIndexStatement("idx", "t", cols, false), expected);
}

TEST(PgDumpTest, BuildCreateIndexStatementUnique) {
    std::vector<std::string> cols = {"a"};
    const std::string expected = "CREATE UNIQUE INDEX \"idx\" ON \"t\" (\"a\");\n";
    EXPECT_EQ(pt::BuildCreateIndexStatement("idx", "t", cols, true), expected);
}

TEST(PgDumpTest, BuildCreateIndexStatementEmptyColumnsEmitsParens) {
    const std::string expected = "CREATE INDEX \"idx\" ON \"t\" ();\n";
    EXPECT_EQ(pt::BuildCreateIndexStatement("idx", "t", {}, false), expected);
}

TEST(PgDumpTest, BuildDropIndexStatement) {
    EXPECT_EQ(pt::BuildDropIndexStatement("idx"), "DROP INDEX IF EXISTS \"idx\";\n");
}

// ---------------------------------------------------------------------------
// Sequence builders.
// ---------------------------------------------------------------------------

TEST(PgDumpTest, BuildCreateSequenceStatementAllOptionsNoCycle) {
    pt::SequenceOptions opts;
    opts.has_start = true;
    opts.has_increment = true;
    opts.has_min = true;
    opts.has_max = true;
    opts.has_cache = true;
    opts.start = 1;
    opts.increment = 1;
    opts.min_value = 1;
    opts.max_value = 1000;
    opts.cache = 1;
    opts.cycle = false;
    const std::string expected =
        "CREATE SEQUENCE \"s\" START WITH 1 INCREMENT BY 1 MINVALUE 1 MAXVALUE 1000 "
        "CACHE 1 NO CYCLE;\n";
    EXPECT_EQ(pt::BuildCreateSequenceStatement("s", opts), expected);
}

TEST(PgDumpTest, BuildCreateSequenceStatementCycle) {
    pt::SequenceOptions opts;
    opts.has_start = true;
    opts.start = 5;
    opts.cycle = true;
    const std::string expected = "CREATE SEQUENCE \"s\" START WITH 5 NO CYCLE;\n";
    // cycle=true must be emitted; other flags remain absent.
    const std::string actual = pt::BuildCreateSequenceStatement("s", opts);
    EXPECT_NE(actual.find("CYCLE"), std::string::npos);
    EXPECT_NE(actual.find("START WITH 5"), std::string::npos);
    EXPECT_EQ(actual.find("NO CYCLE"), std::string::npos);
}

TEST(PgDumpTest, BuildCreateSequenceStatementNoOptionsDefaultsToNoCycle) {
    pt::SequenceOptions opts;
    const std::string expected = "CREATE SEQUENCE \"s\" NO CYCLE;\n";
    EXPECT_EQ(pt::BuildCreateSequenceStatement("s", opts), expected);
}

TEST(PgDumpTest, BuildCreateSequenceStatementNegativeValues) {
    pt::SequenceOptions opts;
    opts.has_increment = true;
    opts.increment = -1;
    const std::string actual = pt::BuildCreateSequenceStatement("s", opts);
    EXPECT_NE(actual.find("INCREMENT BY -1"), std::string::npos);
}

TEST(PgDumpTest, BuildDropSequenceStatement) {
    EXPECT_EQ(pt::BuildDropSequenceStatement("s"), "DROP SEQUENCE IF EXISTS \"s\";\n");
}

// ---------------------------------------------------------------------------
// View builders.
// ---------------------------------------------------------------------------

TEST(PgDumpTest, BuildCreateViewStatement) {
    const std::string def = "SELECT * FROM t WHERE x > 0";
    const std::string expected = "CREATE VIEW \"v\" AS " + def + ";\n";
    EXPECT_EQ(pt::BuildCreateViewStatement("v", def), expected);
}

TEST(PgDumpTest, BuildCreateViewStatementEmptyDefinition) {
    EXPECT_EQ(pt::BuildCreateViewStatement("v", ""), "CREATE VIEW \"v\" AS ;\n");
}

TEST(PgDumpTest, BuildDropViewStatement) {
    EXPECT_EQ(pt::BuildDropViewStatement("v"), "DROP VIEW IF EXISTS \"v\";\n");
}

// ---------------------------------------------------------------------------
// Grant builders.
// ---------------------------------------------------------------------------

TEST(PgDumpTest, BuildGrantStatementNamedRole) {
    const std::string expected = "GRANT SELECT ON \"t\" TO \"alice\";\n";
    EXPECT_EQ(pt::BuildGrantStatement("SELECT", "t", "alice"), expected);
}

TEST(PgDumpTest, BuildGrantStatementEmptyRoleBecomesPublic) {
    const std::string expected = "GRANT SELECT ON \"t\" TO PUBLIC;\n";
    EXPECT_EQ(pt::BuildGrantStatement("SELECT", "t", ""), expected);
}

TEST(PgDumpTest, BuildGrantStatementMultiplePrivileges) {
    const std::string expected = "GRANT SELECT, INSERT ON \"t\" TO \"bob\";\n";
    EXPECT_EQ(pt::BuildGrantStatement("SELECT, INSERT", "t", "bob"), expected);
}

// ---------------------------------------------------------------------------
// SequenceOptions default values.
// ---------------------------------------------------------------------------

TEST(PgDumpTest, SequenceOptionsDefaultsAllFalse) {
    pt::SequenceOptions opts;
    EXPECT_FALSE(opts.has_start);
    EXPECT_FALSE(opts.has_increment);
    EXPECT_FALSE(opts.has_min);
    EXPECT_FALSE(opts.has_max);
    EXPECT_FALSE(opts.has_cache);
    EXPECT_FALSE(opts.cycle);
    EXPECT_EQ(opts.start, 0);
    EXPECT_EQ(opts.increment, 0);
    EXPECT_EQ(opts.min_value, 0);
    EXPECT_EQ(opts.max_value, 0);
    EXPECT_EQ(opts.cache, 0);
}

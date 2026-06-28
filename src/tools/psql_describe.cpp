// psql_describe.cpp — SQL builders for psql backslash meta-commands.
//
// Converted from PostgreSQL 15's src/bin/psql/describe.c. Each function
// returns a SELECT statement against pg_catalog that the psql client sends
// to the server via the simple query protocol.
#include "pgcpp/tools/psql_describe.hpp"

namespace pgcpp::tools {

// Escape a single-quoted SQL string literal: ' -> ''.
// Used to embed user-provided patterns safely into the generated SQL.
// We keep the escape minimal because psql patterns use _ and % (the LIKE
// wildcards) — those characters are passed through unchanged.
namespace {

std::string QuoteLiteral(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (char c : s) {
        if (c == '\'') {
            out.push_back('\'');
            out.push_back('\'');
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

}  // namespace

std::string BuildListTablesSql(const std::string& pattern) {
    std::string sql;
    sql += "SELECT n.nspname AS \"Schema\", c.relname AS \"Name\", ";
    sql += "CASE c.relkind WHEN 'r' THEN 'table' ";
    sql += "WHEN 'p' THEN 'partitioned table' END AS \"Type\" ";
    sql += "FROM pg_catalog.pg_class c ";
    sql += "LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace ";
    sql += "WHERE c.relkind IN ('r','p') ";
    sql += "AND n.nspname <> 'pg_catalog' ";
    sql += "AND n.nspname <> 'information_schema' ";
    sql += "AND n.nspname !~ '^pg_toast'";
    if (!pattern.empty()) {
        sql += " AND c.relname LIKE " + QuoteLiteral(pattern);
    }
    sql += " ORDER BY 1, 2;";
    return sql;
}

std::string BuildListViewsSql(const std::string& pattern) {
    std::string sql;
    sql += "SELECT n.nspname AS \"Schema\", c.relname AS \"Name\", 'view' AS \"Type\" ";
    sql += "FROM pg_catalog.pg_class c ";
    sql += "LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace ";
    sql += "WHERE c.relkind = 'v' ";
    sql += "AND n.nspname <> 'pg_catalog' ";
    sql += "AND n.nspname <> 'information_schema'";
    if (!pattern.empty()) {
        sql += " AND c.relname LIKE " + QuoteLiteral(pattern);
    }
    sql += " ORDER BY 1, 2;";
    return sql;
}

std::string BuildListDatabasesSql() {
    // PostgreSQL's \l uses pg_database; we keep the same shape so the
    // server can answer once it implements pg_database as a queryable
    // relation.
    std::string sql;
    sql += "SELECT d.datname AS \"Name\", ";
    sql += "pg_catalog.pg_get_userbyid(d.datdba) AS \"Owner\", ";
    sql += "pg_catalog.pg_encoding_to_char(d.encoding) AS \"Encoding\" ";
    sql += "FROM pg_catalog.pg_database d ";
    sql += "ORDER BY 1;";
    return sql;
}

std::string BuildListRolesSql() {
    // PostgreSQL exposes roles through the pg_roles view (which is built
    // on top of pg_authid). We keep the same FROM clause.
    std::string sql;
    sql += "SELECT r.rolname AS \"Role\", ";
    sql += "CASE WHEN r.rolsuper THEN 'yes' ELSE 'no' END AS \"Superuser\", ";
    sql += "CASE WHEN r.rolcreaterole THEN 'yes' ELSE 'no' END AS \"Create role\" ";
    sql += "FROM pg_catalog.pg_roles r ";
    sql += "ORDER BY 1;";
    return sql;
}

std::string BuildDescribeRelationSql(const std::string& name) {
    // Equivalent to PostgreSQL's \d <name>: list columns of a relation.
    std::string sql;
    sql += "SELECT a.attname AS \"Column\", ";
    sql += "pg_catalog.format_type(a.atttypid, a.atttypmod) AS \"Type\", ";
    sql += "CASE WHEN a.attnotnull THEN 'not null' ELSE '' END AS \"Nullable\" ";
    sql += "FROM pg_catalog.pg_attribute a ";
    sql += "WHERE a.attrelid = " + QuoteLiteral(name) + "::regclass ";
    sql += "AND a.attnum > 0 ";
    sql += "AND NOT a.attisdropped ";
    sql += "ORDER BY a.attnum;";
    return sql;
}

}  // namespace pgcpp::tools

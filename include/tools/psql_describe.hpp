// psql_describe.h — SQL builders backing psql backslash meta-commands.
//
// Converted from PostgreSQL 15's src/bin/psql/describe.c.
//
// Each function returns the SQL string that psql sends to the server to
// implement the corresponding backslash command (\dt, \dv, \l, \du, \d).
// Keeping the SQL generation in pure functions allows unit-testing without
// a running server.
#pragma once

#include <string>

namespace pgcpp::tools {

// BuildListTablesSql — \dt [pattern]: list user tables.
// When `pattern` is non-empty, it is matched against the table name with
// LIKE.
std::string BuildListTablesSql(const std::string& pattern);

// BuildListViewsSql — \dv [pattern]: list user views.
std::string BuildListViewsSql(const std::string& pattern);

// BuildListDatabasesSql — \l: list databases.
std::string BuildListDatabasesSql();

// BuildListRolesSql — \du: list roles.
std::string BuildListRolesSql();

// BuildDescribeRelationSql — \d <name>: describe a relation's columns.
std::string BuildDescribeRelationSql(const std::string& name);

}  // namespace pgcpp::tools

// parse_utilcmd.h — DDL parse-analysis entry points.
//
// Converted from PostgreSQL 15's src/include/parser/parse_utilcmd.h.
// Provides transformCreateStmt / transformAlterTableStmt / transformIndexStmt,
// which validate types, cook column defaults and CHECK constraints, and
// reject duplicate column names at parse-analysis time.
//
// The transformed statements still wrap as CMD_UTILITY Query nodes — the
// heavy lifting (creating storage, catalog entries) is done later by
// ProcessUtility. The goal is to surface parse-time errors early and to
// produce cooked expression trees that the executor can apply directly.
#pragma once

#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/parser/parse_node.hpp"
#include "pgcpp/parser/parsenodes.hpp"

namespace mytoydb::parser {

using mytoydb::nodes::Node;

// transformCreateStmt — transform a CREATE TABLE statement.
// Resolves column types, processes DEFAULT/CHECK constraints, validates
// column name uniqueness, and wraps as a CMD_UTILITY Query.
Query* transformCreateStmt(ParseState* pstate, CreateStmt* stmt);

// transformAlterTableStmt — transform an ALTER TABLE statement.
// Recursively transforms ADD COLUMN subcommands (resolving types, defaults,
// and CHECK constraints the same way transformCreateStmt does).
Query* transformAlterTableStmt(ParseState* pstate, AlterTableStmt* stmt);

// transformIndexStmt — transform a CREATE INDEX statement.
// Validates that index parameters are well-formed and wraps as a
// CMD_UTILITY Query. (Type resolution for expression indexes is deferred
// to execution time.)
Query* transformIndexStmt(ParseState* pstate, IndexStmt* stmt);

}  // namespace mytoydb::parser

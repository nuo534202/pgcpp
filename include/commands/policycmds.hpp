// policycmds.h — CREATE / ALTER / DROP POLICY (P3-13 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/policy.c.
//
// Row-Level Security (RLS) policies restrict which rows are visible or
// modifiable by which roles. In PostgreSQL a policy is stored in pg_policy
// and applied by the rewriter (rewrite/rowsecurity.c) when RLS is enabled
// on the table.
//
// pgcpp's skeleton implementation parses CREATE/ALTER/DROP POLICY and
// dispatches to these handlers. Persistence (pg_policy rows) and rewriter
// integration land in P3-3 (RLS rewriter); until then the handlers validate
// the parse tree and return the command tag.
#pragma once

#include <string>

namespace pgcpp::parser {
class CreatePolicyStmt;
class AlterPolicyStmt;
class DropPolicyStmt;
}  // namespace pgcpp::parser

namespace pgcpp::commands {

// CreatePolicy — execute CREATE POLICY.
// Validates the statement and (in a full implementation) inserts a pg_policy
// row. Returns the command tag "CREATE POLICY".
std::string CreatePolicy(parser::CreatePolicyStmt* stmt);

// AlterPolicy — execute ALTER POLICY.
// Validates the statement and (in a full implementation) updates the
// matching pg_policy row. Returns "ALTER POLICY".
std::string AlterPolicy(parser::AlterPolicyStmt* stmt);

// DropPolicy — execute DROP POLICY.
// Validates the statement and (in a full implementation) removes the
// matching pg_policy row. Honors IF EXISTS. Returns "DROP POLICY".
std::string DropPolicy(parser::DropPolicyStmt* stmt);

}  // namespace pgcpp::commands

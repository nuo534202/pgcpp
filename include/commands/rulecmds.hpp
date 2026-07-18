// rulecmds.h — CREATE / ALTER / DROP RULE (P3-13 commands module).
//
// Converted from PostgreSQL 15's src/backend/rewrite/rewriteDefine.c
// (the rule definition commands).
//
// Rules are query-rewrite triggers stored in pg_rewrite. CREATE RULE
// inserts a pg_rewrite row attached to a relation. The rewriter
// (rewrite/rewrite_handler.cpp) consults pg_rewrite during query
// analysis to substitute rule actions for the original statement.
//
// pgcpp's skeleton implementation parses the statements and dispatches to
// these handlers. Persistence (pg_rewrite rows) and rewriter integration
// land in P3-3 (RLS rewriter); until then the handlers validate the parse
// tree and return the command tag.
#pragma once

#include <string>

namespace pgcpp::parser {
class CreateRuleStmt;
class AlterRuleStmt;
class DropRuleStmt;
}  // namespace pgcpp::parser

namespace pgcpp::commands {

// CreateRule — execute CREATE [OR REPLACE] RULE.
std::string CreateRule(parser::CreateRuleStmt* stmt);

// AlterRule — execute ALTER RULE.
std::string AlterRule(parser::AlterRuleStmt* stmt);

// DropRule — execute DROP RULE.
std::string DropRule(parser::DropRuleStmt* stmt);

}  // namespace pgcpp::commands

// rulecmds.cpp — CREATE / ALTER / DROP RULE implementation (P3-13).
//
// Converted from PostgreSQL 15's src/backend/rewrite/rewriteDefine.c
// (the rule definition commands).
//
// Skeleton implementation: validates the parse tree and returns the command
// tag. pg_rewrite persistence and rewriter integration land in P3-3.
#include "commands/rulecmds.hpp"

#include <string>

#include "common/error/elog.hpp"
#include "parser/parsenodes.hpp"

namespace pgcpp::commands {

using pgcpp::parser::AlterRuleStmt;
using pgcpp::parser::CreateRuleStmt;
using pgcpp::parser::DropRuleStmt;

std::string CreateRule(CreateRuleStmt* stmt) {
    if (stmt == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "CREATE RULE: null statement");
    }
    if (stmt->rule_name.empty()) {
        ereport(pgcpp::error::LogLevel::kError, "CREATE RULE: missing rule name");
    }
    if (stmt->relation == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "CREATE RULE: missing target relation");
    }
    // pg_rewrite insert and rewriter integration land in P3-3.
    return "CREATE RULE";
}

std::string AlterRule(AlterRuleStmt* stmt) {
    if (stmt == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "ALTER RULE: null statement");
    }
    if (stmt->rule_name.empty()) {
        ereport(pgcpp::error::LogLevel::kError, "ALTER RULE: missing rule name");
    }
    if (stmt->relation == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "ALTER RULE: missing target relation");
    }
    return "ALTER RULE";
}

std::string DropRule(DropRuleStmt* stmt) {
    if (stmt == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "DROP RULE: null statement");
    }
    if (stmt->rule_names.empty()) {
        ereport(pgcpp::error::LogLevel::kError, "DROP RULE: missing rule name");
    }
    if (stmt->relation == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "DROP RULE: missing target relation");
    }
    return "DROP RULE";
}

}  // namespace pgcpp::commands

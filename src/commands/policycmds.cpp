// policycmds.cpp — CREATE / ALTER / DROP POLICY implementation (P3-13).
//
// Converted from PostgreSQL 15's src/backend/commands/policy.c.
//
// Skeleton implementation: validates the parse tree and returns the command
// tag. Persistence (pg_policy rows) and rewriter integration land in P3-3.
#include "commands/policycmds.hpp"

#include <string>

#include "common/error/elog.hpp"
#include "parser/parsenodes.hpp"

namespace pgcpp::commands {

using pgcpp::parser::AlterPolicyStmt;
using pgcpp::parser::CreatePolicyStmt;
using pgcpp::parser::DropPolicyStmt;

std::string CreatePolicy(CreatePolicyStmt* stmt) {
    if (stmt == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "CREATE POLICY: null statement");
    }
    if (stmt->policy_name.empty()) {
        ereport(pgcpp::error::LogLevel::kError, "CREATE POLICY: missing policy name");
    }
    if (stmt->table == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "CREATE POLICY: missing target table");
    }
    // Persistence (pg_policy insert) and rewriter integration land in P3-3.
    return "CREATE POLICY";
}

std::string AlterPolicy(AlterPolicyStmt* stmt) {
    if (stmt == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "ALTER POLICY: null statement");
    }
    if (stmt->policy_name.empty()) {
        ereport(pgcpp::error::LogLevel::kError, "ALTER POLICY: missing policy name");
    }
    if (stmt->table == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "ALTER POLICY: missing target table");
    }
    return "ALTER POLICY";
}

std::string DropPolicy(DropPolicyStmt* stmt) {
    if (stmt == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "DROP POLICY: null statement");
    }
    if (stmt->policy_name.empty()) {
        ereport(pgcpp::error::LogLevel::kError, "DROP POLICY: missing policy name");
    }
    if (stmt->table == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "DROP POLICY: missing target table");
    }
    // IF EXISTS handling and CASCADE behavior land with pg_policy persistence.
    return "DROP POLICY";
}

}  // namespace pgcpp::commands

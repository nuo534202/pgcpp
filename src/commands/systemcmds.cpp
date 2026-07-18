// systemcmds.cpp — ALTER SYSTEM implementation (P3-13).
//
// Converted from PostgreSQL 15's src/backend/commands/variable.c
// (the ALTER SYSTEM SET/RESET portion).
//
// Skeleton implementation: validates the parse tree and returns the command
// tag. Persistence to postgresql.auto.conf lands in a future phase.
#include "commands/systemcmds.hpp"

#include <string>

#include "common/error/elog.hpp"
#include "parser/parsenodes.hpp"

namespace pgcpp::commands {

using pgcpp::parser::AlterSystemStmt;

std::string AlterSystem(AlterSystemStmt* stmt) {
    if (stmt == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "ALTER SYSTEM: null statement");
    }
    // For RESET ALL, no GUC name is required. For SET/RESET, the name is required.
    if (stmt->kind != AlterSystemStmt::Kind::kResetAll && stmt->name.empty()) {
        ereport(pgcpp::error::LogLevel::kError,
                "ALTER SYSTEM: missing configuration parameter name");
    }
    // postgresql.auto.conf write and SIGHUP to postmaster land in a future phase.
    return "ALTER SYSTEM";
}

}  // namespace pgcpp::commands

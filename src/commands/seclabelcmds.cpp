// seclabelcmds.cpp — SECURITY LABEL implementation (P3-13).
//
// Converted from PostgreSQL 15's src/backend/commands/seclabel.c.
//
// Skeleton implementation: validates the parse tree and returns the command
// tag. Provider hooks and pg_seclabel persistence land in a future phase.
#include "commands/seclabelcmds.hpp"

#include <string>

#include "common/error/elog.hpp"
#include "parser/parsenodes.hpp"

namespace pgcpp::commands {

using pgcpp::parser::SecLabelStmt;

std::string SecLabel(SecLabelStmt* stmt) {
    if (stmt == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "SECURITY LABEL: null statement");
    }
    if (stmt->object.empty()) {
        ereport(pgcpp::error::LogLevel::kError, "SECURITY LABEL: missing target object");
    }
    // pg_seclabel insert (or delete if label is empty) lands with provider
    // registration support in a future phase.
    return "SECURITY LABEL";
}

}  // namespace pgcpp::commands

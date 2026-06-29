// operatorcmds.cpp — CREATE OPERATOR implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/operatorcmds.c.
// pgcpp has no user-defined operator system yet; this is a stub.
#include "commands/operatorcmds.hpp"

#include <string>

#include "parser/parsenodes.hpp"

namespace pgcpp::commands {

using pgcpp::parser::CreateStmt;

std::string DefineOperator(CreateStmt* stmt) {
    (void)stmt;  // Stub.
    return "CREATE OPERATOR";
}

}  // namespace pgcpp::commands

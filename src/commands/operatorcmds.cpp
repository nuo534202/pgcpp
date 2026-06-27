// operatorcmds.cpp — CREATE OPERATOR implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/operatorcmds.c.
// MyToyDB has no user-defined operator system yet; this is a stub.
#include "mytoydb/commands/operatorcmds.hpp"

#include <string>

#include "mytoydb/parser/parsenodes.hpp"

namespace mytoydb::commands {

using mytoydb::parser::CreateStmt;

std::string DefineOperator(CreateStmt* stmt) {
    (void)stmt;  // Stub.
    return "CREATE OPERATOR";
}

}  // namespace mytoydb::commands

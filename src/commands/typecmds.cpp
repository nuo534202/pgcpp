// typecmds.cpp — CREATE TYPE implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/typecmds.c.
// pgcpp has no user-defined type system yet; this is a stub.
#include "commands/typecmds.hpp"

#include <string>

#include "parser/parsenodes.hpp"

namespace pgcpp::commands {

using pgcpp::parser::CreateStmt;

std::string DefineType(CreateStmt* stmt) {
    (void)stmt;  // Stub.
    return "CREATE TYPE";
}

}  // namespace pgcpp::commands

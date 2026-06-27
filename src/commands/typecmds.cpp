// typecmds.cpp — CREATE TYPE implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/typecmds.c.
// MyToyDB has no user-defined type system yet; this is a stub.
#include "mytoydb/commands/typecmds.hpp"

#include <string>

#include "mytoydb/parser/parsenodes.hpp"

namespace mytoydb::commands {

using mytoydb::parser::CreateStmt;

std::string DefineType(CreateStmt* stmt) {
    (void)stmt;  // Stub.
    return "CREATE TYPE";
}

}  // namespace mytoydb::commands

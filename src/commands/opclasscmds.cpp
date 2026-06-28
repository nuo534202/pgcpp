// opclasscmds.cpp — CREATE OPERATOR CLASS implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/opclasscmds.c.
// MyToyDB has a fixed B-tree operator class; this is a stub.
#include "pgcpp/commands/opclasscmds.hpp"

#include <string>

#include "pgcpp/parser/parsenodes.hpp"

namespace mytoydb::commands {

using mytoydb::parser::CreateStmt;

std::string DefineOpClass(CreateStmt* stmt) {
    (void)stmt;  // Stub.
    return "CREATE OPERATOR CLASS";
}

}  // namespace mytoydb::commands

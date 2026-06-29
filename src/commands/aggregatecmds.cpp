// aggregatecmds.cpp — CREATE AGGREGATE implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/aggregatecmds.c.
// pgcpp has fixed built-in aggregates (count, sum, avg, min, max);
// this is a stub for user-defined aggregates.
#include "commands/aggregatecmds.hpp"

#include <string>

#include "parser/parsenodes.hpp"

namespace pgcpp::commands {

using pgcpp::parser::CreateStmt;

std::string DefineAggregate(CreateStmt* stmt) {
    (void)stmt;  // Stub.
    return "CREATE AGGREGATE";
}

}  // namespace pgcpp::commands

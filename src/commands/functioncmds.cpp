// functioncmds.cpp — CREATE FUNCTION implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/functioncmds.c.
// pgcpp doesn't yet support user-defined functions; this stub
// acknowledges the request without persisting a pg_proc row.
#include "commands/functioncmds.hpp"

#include <string>

#include "parser/parsenodes.hpp"

namespace pgcpp::commands {

using pgcpp::parser::CreateFunctionStmt;

std::string CreateFunction(CreateFunctionStmt* stmt) {
    if (stmt == nullptr)
        return "CREATE FUNCTION";
    // Stub: real PostgreSQL creates a pg_proc row and validates the
    // function body against the language handler.
    return stmt->is_procedure ? "CREATE PROCEDURE" : "CREATE FUNCTION";
}

}  // namespace pgcpp::commands

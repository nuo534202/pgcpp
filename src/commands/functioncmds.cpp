// functioncmds.cpp — CREATE FUNCTION implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/functioncmds.c.
// MyToyDB doesn't yet support user-defined functions; this stub
// acknowledges the request without persisting a pg_proc row.
#include "mytoydb/commands/functioncmds.hpp"

#include <string>

#include "mytoydb/parser/parsenodes.hpp"

namespace mytoydb::commands {

using mytoydb::parser::CreateFunctionStmt;

std::string CreateFunction(CreateFunctionStmt* stmt) {
    if (stmt == nullptr)
        return "CREATE FUNCTION";
    // Stub: real PostgreSQL creates a pg_proc row and validates the
    // function body against the language handler.
    return stmt->is_procedure ? "CREATE PROCEDURE" : "CREATE FUNCTION";
}

}  // namespace mytoydb::commands

// vacuum.cpp — VACUUM command implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/vacuum.c.
// pgcpp's MVCC implementation currently reclaims dead tuples eagerly
// during DML (heap_insert/heap_delete/heap_update), so VACUUM is a
// no-op that simply returns the command tag.
#include "commands/vacuum.hpp"

#include <string>

#include "parser/parsenodes.hpp"

namespace pgcpp::commands {

using pgcpp::parser::VacuumStmt;

std::string ExecVacuum(VacuumStmt* stmt) {
    if (stmt == nullptr)
        return "VACUUM";
    return stmt->is_vacuumcmd ? "VACUUM" : "ANALYZE";
}

}  // namespace pgcpp::commands

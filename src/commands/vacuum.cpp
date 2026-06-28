// vacuum.cpp — VACUUM command implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/vacuum.c.
// MyToyDB's MVCC implementation currently reclaims dead tuples eagerly
// during DML (heap_insert/heap_delete/heap_update), so VACUUM is a
// no-op that simply returns the command tag.
#include "pgcpp/commands/vacuum.hpp"

#include <string>

#include "pgcpp/parser/parsenodes.hpp"

namespace mytoydb::commands {

using mytoydb::parser::VacuumStmt;

std::string ExecVacuum(VacuumStmt* stmt) {
    if (stmt == nullptr)
        return "VACUUM";
    return stmt->is_vacuumcmd ? "VACUUM" : "ANALYZE";
}

}  // namespace mytoydb::commands

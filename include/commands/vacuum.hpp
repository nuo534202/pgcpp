// vacuum.h — VACUUM command (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/vacuum.c.
// pgcpp's MVCC implementation marks dead tuples in-place during DML
// (heap_delete sets t_xmax; heap_update sets t_xmax + inserts a new
// version). VACUUM reclaims physical space by compacting pages whose
// dead tuples are no longer visible to any running transaction.
#pragma once

#include <string>

namespace pgcpp::parser {
class VacuumStmt;
}  // namespace pgcpp::parser

namespace pgcpp::commands {

// ExecVacuum — execute VACUUM (and ANALYZE when stmt->is_vacuumcmd is
// false). Returns the command tag ("VACUUM" or "ANALYZE").
std::string ExecVacuum(parser::VacuumStmt* stmt);

}  // namespace pgcpp::commands

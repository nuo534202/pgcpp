// vacuum.h — VACUUM command (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/vacuum.c.
// MyToyDB's MVCC implementation currently reclaims dead tuples eagerly
// during DML, so VACUUM is a no-op that returns the command tag.
#pragma once

#include <string>

namespace mytoydb::parser {
class VacuumStmt;
}  // namespace mytoydb::parser

namespace mytoydb::commands {

// ExecVacuum — execute VACUUM (and ANALYZE when stmt->is_vacuumcmd is
// false). Returns the command tag ("VACUUM" or "ANALYZE").
std::string ExecVacuum(parser::VacuumStmt* stmt);

}  // namespace mytoydb::commands

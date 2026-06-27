// analyze.h — ANALYZE command (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/analyze.c.
// Updates pg_statistic for the planner. Currently a no-op stub since
// MyToyDB's planner uses fixed cardinality estimates.
#pragma once

#include <string>

namespace mytoydb::parser {
class VacuumStmt;
}  // namespace mytoydb::parser

namespace mytoydb::commands {

// AnalyzeCommand — execute ANALYZE. Returns "ANALYZE".
// (VacuumStmt carries both VACUUM and ANALYZE; the dispatcher routes
// non-vacuumcmd statements here.)
std::string AnalyzeCommand(parser::VacuumStmt* stmt);

}  // namespace mytoydb::commands

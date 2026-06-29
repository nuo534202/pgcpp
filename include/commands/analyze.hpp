// analyze.h — ANALYZE command (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/analyze.c.
// Updates pg_statistic for the planner. Currently a no-op stub since
// pgcpp's planner uses fixed cardinality estimates.
#pragma once

#include <string>

namespace pgcpp::parser {
class VacuumStmt;
}  // namespace pgcpp::parser

namespace pgcpp::commands {

// AnalyzeCommand — execute ANALYZE. Returns "ANALYZE".
// (VacuumStmt carries both VACUUM and ANALYZE; the dispatcher routes
// non-vacuumcmd statements here.)
std::string AnalyzeCommand(parser::VacuumStmt* stmt);

}  // namespace pgcpp::commands

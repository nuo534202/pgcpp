// analyze.h — ANALYZE command (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/analyze.c.
// Scans heap relations to collect per-column statistics (null fraction,
// average width, distinct count, MCV, histogram) and writes them to
// pg_statistic for the planner/optimizer. Also updates pg_class.relpages
// and reltuples so the cost model has accurate relation sizes.
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

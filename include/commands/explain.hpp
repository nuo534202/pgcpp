// explain.h — EXPLAIN command (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/explain.c.
#pragma once

#include <string>

namespace pgcpp::parser {
class ExplainStmt;
}  // namespace pgcpp::parser

namespace pgcpp::commands {

// ExplainQuery — execute EXPLAIN. Currently a stub that prints a
// placeholder plan description to stdout. Returns "EXPLAIN".
std::string ExplainQuery(parser::ExplainStmt* stmt);

}  // namespace pgcpp::commands

// explain.h — EXPLAIN command (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/explain.c.
#pragma once

#include <string>

namespace mytoydb::parser {
class ExplainStmt;
}  // namespace mytoydb::parser

namespace mytoydb::commands {

// ExplainQuery — execute EXPLAIN. Currently a stub that prints a
// placeholder plan description to stdout. Returns "EXPLAIN".
std::string ExplainQuery(parser::ExplainStmt* stmt);

}  // namespace mytoydb::commands

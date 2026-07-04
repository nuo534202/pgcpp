// explain.h — EXPLAIN command (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/explain.c.
#pragma once

#include <string>

namespace pgcpp::protocol {
class OutputSink;
}  // namespace pgcpp::protocol

namespace pgcpp::parser {
class ExplainStmt;
}  // namespace pgcpp::parser

namespace pgcpp::commands {

// ExplainQuery — execute EXPLAIN. Sends a RowDescription + DataRow stream
// to the client sink so the output appears in the client (psql) instead of
// the server's stdout. Returns "EXPLAIN" as the command tag.
std::string ExplainQuery(parser::ExplainStmt* stmt, protocol::OutputSink* sink);

}  // namespace pgcpp::commands

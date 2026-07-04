// explain.cpp — EXPLAIN command implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/explain.c.
// pgcpp's EXPLAIN sends a placeholder plan description to the client via
// the protocol sink (RowDescription + DataRow), matching PostgreSQL's
// wire behaviour. Full plan-tree dumping is deferred until the executor
// exposes an ExplainState API.
#include "commands/explain.hpp"

#include <string>
#include <vector>

#include "parser/parsenodes.hpp"
#include "protocol/pqformat.hpp"

namespace pgcpp::commands {

using pgcpp::parser::ExplainStmt;
using pgcpp::protocol::BuildDataRow;
using pgcpp::protocol::BuildRowDescription;
using pgcpp::protocol::MessageType;
using pgcpp::protocol::OutputSink;
using pgcpp::protocol::RowDescriptionField;

std::string ExplainQuery(ExplainStmt* stmt, OutputSink* sink) {
    if (stmt == nullptr)
        return "EXPLAIN";

    // Send RowDescription: a single text column named "QUERY PLAN".
    if (sink != nullptr) {
        RowDescriptionField field;
        field.name = "QUERY PLAN";
        field.type_oid = 25;   // TEXTOID
        field.type_size = -1;  // variable-length
        field.type_mod = -1;
        field.format = 0;  // text
        sink->SendMessage(BuildRowDescription({field}));

        // Send the plan lines as DataRows. Until a real plan-tree dumper
        // exists, emit a placeholder line so the client sees a well-formed
        // EXPLAIN result set.
        sink->SendMessage(BuildDataRow({"(explain output not yet implemented)"}, {false}));
    }
    return "EXPLAIN";
}

}  // namespace pgcpp::commands

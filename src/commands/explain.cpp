// explain.cpp — EXPLAIN command implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/explain.c.
// pgcpp's EXPLAIN currently prints a placeholder plan description
// to stdout; full plan-tree dumping is deferred until the executor
// exposes an ExplainState API.
#include "pgcpp/commands/explain.hpp"

#include <iostream>
#include <string>

#include "pgcpp/parser/parsenodes.hpp"

namespace pgcpp::commands {

using pgcpp::parser::ExplainStmt;

std::string ExplainQuery(ExplainStmt* stmt) {
    if (stmt == nullptr)
        return "EXPLAIN";
    // The query field carries the planned statement; without an
    // ExplainState we just acknowledge the request. Real output would
    // walk the Plan tree and print each node's cost / rows / filter.
    std::cout << "QUERY PLAN\n"
              << "----------\n"
              << "(explain output not yet implemented)\n";
    return "EXPLAIN";
}

}  // namespace pgcpp::commands

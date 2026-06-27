// explain.cpp — EXPLAIN command implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/explain.c.
// MyToyDB's EXPLAIN currently prints a placeholder plan description
// to stdout; full plan-tree dumping is deferred until the executor
// exposes an ExplainState API.
#include "mytoydb/commands/explain.hpp"

#include <iostream>
#include <string>

#include "mytoydb/parser/parsenodes.hpp"

namespace mytoydb::commands {

using mytoydb::parser::ExplainStmt;

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

}  // namespace mytoydb::commands

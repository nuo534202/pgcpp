// analyze.h — Parse analysis entry point.
//
// Converted from PostgreSQL 15's src/include/parser/analyze.h.
// Provides parse_analyze(), the public entry point that transforms
// RawStmt parse trees into Query nodes.
#pragma once

#include <string>
#include <vector>

#include "mytoydb/common/containers/node.hpp"
#include "mytoydb/parser/parse_node.hpp"
#include "mytoydb/parser/parsenodes.hpp"

namespace mytoydb::parser {

using mytoydb::nodes::Node;

// parse_analyze — transform a list of RawStmt nodes into a list of Query nodes.
// This is the main entry point for parse analysis.
// Takes const ref to avoid copying the vector (a copy would leak if ereport
// fires during analysis, since longjmp bypasses the copy's destructor).
std::vector<Query*> parse_analyze(const std::vector<RawStmt*>& parse_trees,
                                  const char* source_string);

// parse_analyze_varparams — like parse_analyze but allows variable parameters.
std::vector<Query*> parse_analyze_varparams(const std::vector<RawStmt*>& parse_trees,
                                            const char* source_string);

// transformStmt — transform a single statement into a Query.
Query* transformStmt(ParseState* pstate, Node* stmt);

// transformTopLevelStmt — transform a top-level statement.
Query* transformTopLevelStmt(ParseState* pstate, RawStmt* parse_tree);

}  // namespace mytoydb::parser

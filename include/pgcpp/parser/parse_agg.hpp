// parse_agg.h — Aggregate function handling for parse analysis.
//
// Converted from PostgreSQL 15's src/include/parser/parse_agg.h.
#pragma once

#include <vector>

#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/parser/parse_node.hpp"

namespace mytoydb::parser {

using mytoydb::nodes::Node;

// transformAggregateCall — transform an aggregate function call.
// Called from transformFuncCall when the function is an aggregate.
Node* transformAggregateCall(ParseState* pstate, Aggref* agg, std::vector<Node*>& args,
                             int location);

// parseCheckAggregates — check aggregate placement and set query flags.
// Called after the target list, WHERE, HAVING are all transformed.
void parseCheckAggregates(ParseState* pstate, Query* qry);

// count_agg_clauses — count aggregate references in an expression tree.
// Used to detect aggregates in WHERE clause (which is invalid).
int count_agg_clauses(Node* node);

}  // namespace mytoydb::parser

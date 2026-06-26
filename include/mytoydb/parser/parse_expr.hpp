// parse_expr.h — Expression transformation for parse analysis.
//
// Converted from PostgreSQL 15's src/include/parser/parse_expr.h.
#pragma once

#include "mytoydb/common/containers/node.hpp"
#include "mytoydb/parser/parse_node.hpp"

namespace mytoydb::parser {

using mytoydb::nodes::Node;

// transformExpr — transform a raw expression into a transformed expression.
// Dispatches based on the node type of the input expression.
Node* transformExpr(ParseState* pstate, Node* expr, ParseExprKind exprKind);

// transformExprRecurse — internal recursive transform dispatch.
Node* transformExprRecurse(ParseState* pstate, Node* expr);

}  // namespace mytoydb::parser

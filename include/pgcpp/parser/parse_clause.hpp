// parse_clause.h — Clause transformation for parse analysis.
//
// Converted from PostgreSQL 15's src/include/parser/parse_clause.h.
#pragma once

#include <vector>

#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/parser/parse_node.hpp"
#include "pgcpp/parser/primnodes.hpp"

namespace pgcpp::parser {

using pgcpp::nodes::Node;

// transformFromClause — transform the FROM clause into a join tree.
// Returns a FromExpr node (the root of the join tree).
// Takes const ref to avoid copying the vector (a copy would leak if ereport
// fires during transformation, since longjmp bypasses the copy's destructor).
Node* transformFromClause(ParseState* pstate, const std::vector<Node*>& frmList);

// transformFromClauseItem — transform one item in the FROM clause.
// Returns the join tree node (RangeTblRef, JoinExpr, etc.).
Node* transformFromClauseItem(ParseState* pstate, Node* n, RangeTblEntry** top_rte, int* top_rti,
                              std::vector<ParseNamespaceItem*>& nsitem);

// transformWhereClause — transform a WHERE/HAVING clause.
Node* transformWhereClause(ParseState* pstate, Node* clause, ParseExprKind exprKind,
                           const char* constructName);

// transformLimitClause — transform a LIMIT/OFFSET clause.
Node* transformLimitClause(ParseState* pstate, Node* clause, ParseExprKind exprKind,
                           const char* constructName);

}  // namespace pgcpp::parser

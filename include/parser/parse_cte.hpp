// parse_cte.h — WITH clause / CTE parse-analysis entry point.
//
// Converted from PostgreSQL 15's src/include/parser/parse_cte.h.
// transformWithClause analyzes each CommonTableExpr's subquery and adds
// the CTE to pstate->p_ctenamespace so the FROM clause resolver can find
// it by name.
#pragma once

#include "common/containers/node.hpp"
#include "parser/parse_node.hpp"
#include "parser/parsenodes.hpp"

namespace pgcpp::parser {

// transformWithClause — analyze each CTE in `with_clause` and add it to
// pstate->p_ctenamespace. No-op when with_clause is nullptr.
//
// For non-recursive WITH, each CTE is added to p_ctenamespace before its
// subquery is analyzed, so later CTEs can reference earlier ones (and the
// main query can reference any of them). For WITH RECURSIVE, each CTE is
// added to p_future_ctes first (so it can reference itself recursively),
// then promoted to p_ctenamespace after analysis.
void transformWithClause(ParseState* pstate, WithClause* with_clause);

}  // namespace pgcpp::parser

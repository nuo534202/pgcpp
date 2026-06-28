// parse_cte.cpp — WITH clause / CTE parse-analysis entry point.
//
// Converted from PostgreSQL 15's src/backend/parser/parse_cte.c.
//
// Implements transformWithClause: analyzes each CommonTableExpr's subquery
// into a Query tree and adds the CTE to pstate->p_ctenamespace so the FROM
// clause resolver (parse_clause.cpp) can find CTE references by name.
//
// RECURSIVE handling: a recursive CTE can reference itself in its own
// subquery. PostgreSQL handles this by adding the CTE to p_future_ctes
// first (visible only to its own analysis), then promoting it to
// p_ctenamespace after analysis completes. We do the same here, though
// full recursive-execution support (worktable scan, etc.) is out of scope
// for this task — only the parse-analysis plumbing is implemented.
#include "pgcpp/parser/parse_cte.hpp"

#include <string>
#include <vector>

#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/parser/analyze.hpp"
#include "pgcpp/parser/parse_node.hpp"
#include "pgcpp/parser/parsenodes.hpp"

namespace pgcpp::parser {

using pgcpp::nodes::Node;
using pgcpp::nodes::NodeTag;
using pgcpp::nodes::nodeTag;

namespace {

// Remove `target` from `vec` if present (first match only).
void RemoveFromVector(std::vector<Node*>& vec, Node* target) {
    for (auto it = vec.begin(); it != vec.end(); ++it) {
        if (*it == target) {
            vec.erase(it);
            return;
        }
    }
}

// analyzeCTE — transform a single CTE's ctequery (a SelectStmt) into an
// analyzed Query, using a child ParseState so the CTE's own FROM/WHERE
// resolution does not leak into the parent.
void AnalyzeCTE(ParseState* pstate, CommonTableExpr* cte) {
    if (cte == nullptr || cte->ctequery == nullptr)
        return;

    // The CTE's subquery must be a SelectStmt at this point. (Other forms,
    // like INSERT/UPDATE/DELETE inside WITH, are out of scope for this
    // task — PostgreSQL supports them but ClickBench does not use them.)
    if (cte->ctequery->GetTag() != NodeTag::kSelectStmt) {
        ereport(pgcpp::error::LogLevel::kError,
                "non-SELECT statement in WITH clause is not supported");
    }

    ParseState* sub_pstate = make_parsestate(pstate);
    sub_pstate->p_parent_cte = cte;
    Query* q = transformStmt(sub_pstate, cte->ctequery);
    free_parsestate(sub_pstate);

    cte->ctequery = q;
}

}  // namespace

// ---------------------------------------------------------------------------
// transformWithClause
// ---------------------------------------------------------------------------

void transformWithClause(ParseState* pstate, WithClause* with_clause) {
    if (with_clause == nullptr)
        return;

    const bool recursive = with_clause->recursive;

    for (Node* cte_node : with_clause->ctes) {
        if (cte_node == nullptr || cte_node->GetTag() != NodeTag::kCommonTableExpr)
            continue;
        auto* cte = static_cast<CommonTableExpr*>(cte_node);

        cte->cterecursive = recursive;

        if (recursive) {
            // Add to p_future_ctes so the CTE can reference itself during
            // its own analysis. parse_clause.cpp looks up p_future_ctes
            // first when resolving RangeVar names.
            pstate->p_future_ctes.push_back(cte);
        } else {
            // Non-recursive CTEs are visible to later CTEs and to the
            // main query — add to p_ctenamespace immediately.
            pstate->p_ctenamespace.push_back(cte);
        }

        AnalyzeCTE(pstate, cte);

        if (recursive) {
            // Promote from p_future_ctes to p_ctenamespace now that the
            // CTE's analysis is complete.
            RemoveFromVector(pstate->p_future_ctes, cte);
            pstate->p_ctenamespace.push_back(cte);
        }
    }
}

}  // namespace pgcpp::parser

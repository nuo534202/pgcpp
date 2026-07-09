// rewrite_handler.cpp — Query rewrite system main handler.
//
// Converted from PostgreSQL 15's src/backend/rewrite/rewriteHandler.c.
//
// QueryRewrite is called between parse_analyze and the planner. It:
//   1. Expands view RTEs into subquery RTEs (view substitution)
//   2. Applies Row Level Security policies
//   3. Recursively rewrites subqueries
//
// The core operation is view expansion: when a query's range table contains
// a relation RTE whose relkind is 'v' (view), the RTE is converted to a
// subquery RTE whose subquery is the view's stored SELECT query tree.
#include "rewrite/rewrite_handler.hpp"

#include <string>
#include <vector>

#include "catalog/catalog.hpp"
#include "catalog/pg_class.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "parser/parsenodes.hpp"
#include "parser/primnodes.hpp"

namespace pgcpp::rewrite {

using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::RelKind;
using pgcpp::nodes::makePallocNode;
using pgcpp::nodes::NodeTag;
using pgcpp::parser::Alias;
using pgcpp::parser::FromExpr;
using pgcpp::parser::Node;
using pgcpp::parser::Query;
using pgcpp::parser::RangeTblEntry;
using pgcpp::parser::RangeTblRef;

// Forward declarations of helpers defined in rewrite_define.cpp.
bool IsViewRelation(int relid);

// --- Internal helpers ---

// Check if an RTE references a view (relkind == 'v').
static bool IsViewRTE(const RangeTblEntry* rte) {
    if (rte == nullptr)
        return false;
    if (rte->rtekind != pgcpp::parser::RTEKind::kRelation)
        return false;
    return rte->relkind == static_cast<char>(RelKind::kView);
}

// Deep-copy a Query tree for view expansion. In pgcpp, we use the Clone()
// method on the Query node. This ensures each expansion gets its own copy
// (PostgreSQL uses copyObject to achieve the same).
static Query* CopyQueryTree(const Query* src) {
    if (src == nullptr)
        return nullptr;
    Node* cloned = src->Clone();
    return static_cast<Query*>(cloned);
}

// RewriteView — expand a single view RTE into a subquery RTE.
//
// Replaces the RTE at rt_index (1-based) with a subquery RTE containing
// the view's stored Query tree. The original RTE's alias and eref are
// preserved so column references in the outer query still resolve.
//
// Returns true if the RTE was rewritten, false if it was not a view or
// no view query was found.
bool RewriteView(Query* query, int rt_index) {
    if (query == nullptr)
        return false;
    if (rt_index < 1 || static_cast<size_t>(rt_index) > query->rtable.size())
        return false;

    RangeTblEntry* rte = static_cast<RangeTblEntry*>(query->rtable[rt_index - 1]);
    if (rte == nullptr)
        return false;
    if (!IsViewRTE(rte))
        return false;

    // Look up the view's stored query.
    Query* view_query = RetrieveViewQuery(rte->relid);
    if (view_query == nullptr) {
        // No view query stored — this can happen if the view was created
        // before the rewrite system was implemented. Leave the RTE as-is.
        return false;
    }

    // Deep-copy the view query so multiple expansions are independent.
    Query* subquery = CopyQueryTree(view_query);
    if (subquery == nullptr)
        return false;

    // Convert the RTE from relation to subquery.
    rte->rtekind = pgcpp::parser::RTEKind::kSubquery;
    rte->subquery = subquery;
    // Clear relation-specific fields that don't apply to subqueries.
    rte->relid = 0;
    rte->relkind = 0;
    rte->rellockmode = 0;
    // security_barrier views prevent predicate pushdown for security.
    // pgcpp sets this to false by default (can be extended for security views).

    return true;
}

// RewriteRtable — iterate over all RTEs in the query and expand views.
static void RewriteRtable(Query* query) {
    if (query == nullptr)
        return;

    for (size_t i = 0; i < query->rtable.size(); i++) {
        RangeTblEntry* rte = static_cast<RangeTblEntry*>(query->rtable[i]);
        if (rte == nullptr)
            continue;

        // Expand view RTEs into subquery RTEs.
        if (IsViewRTE(rte)) {
            RewriteView(query, static_cast<int>(i + 1));
        }

        // Recursively rewrite subquery RTEs (handles nested views).
        if (rte->rtekind == pgcpp::parser::RTEKind::kSubquery && rte->subquery != nullptr) {
            QueryRewrite(rte->subquery);
        }
    }
}

// QueryRewrite — main entry point for the query rewrite system.
//
// Applies view expansion and RLS policies to a parsed Query tree.
// Returns the (possibly modified) Query pointer.
Query* QueryRewrite(Query* query) {
    if (query == nullptr)
        return nullptr;

    // Don't rewrite utility statements.
    if (query->command_type == pgcpp::parser::CmdType::kUtility)
        return query;

    // 1. Expand views in the range table.
    RewriteRtable(query);

    // 2. Apply Row Level Security policies.
    for (size_t i = 0; i < query->rtable.size(); i++) {
        RangeTblEntry* rte = static_cast<RangeTblEntry*>(query->rtable[i]);
        if (rte == nullptr)
            continue;
        if (rte->rtekind != pgcpp::parser::RTEKind::kRelation)
            continue;
        if (IsRowSecurityEnabled(rte->relid)) {
            ApplyRowSecurity(query, static_cast<int>(i + 1));
        }
    }

    return query;
}

}  // namespace pgcpp::rewrite

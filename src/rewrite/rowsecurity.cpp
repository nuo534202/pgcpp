// rowsecurity.cpp — Row Level Security policy application.
//
// Converted from PostgreSQL 15's src/backend/rewrite/rowsecurity.c.
//
// Applies RLS policies to queries. When a relation has RLS enabled, the
// policy's qualification expression is added to the query's security_quals
// list on the RTE. The executor evaluates these quals as additional filter
// conditions, restricting which rows are visible.
//
// pgcpp implements a simplified single-policy-per-relation model. PostgreSQL
// supports multiple policies (USING/WITH CHECK, per-command, per-role) which
// can be added as the grammar and catalog evolve.
#include "common/containers/node.hpp"
#include "parser/parsenodes.hpp"
#include "parser/primnodes.hpp"
#include "rewrite/rewrite_handler.hpp"

namespace pgcpp::rewrite {

using pgcpp::nodes::makePallocNode;
using pgcpp::parser::Node;
using pgcpp::parser::Query;
using pgcpp::parser::RangeTblEntry;

// ApplyRowSecurity — inject RLS policy quals into a query.
//
// If the relation at rt_index has RLS enabled and a policy is stored,
// the policy qual is appended to the RTE's security_quals list. This
// causes the executor to filter rows that don't satisfy the policy.
//
// In PostgreSQL, this also handles:
//   - WITH CHECK quals (for INSERT/UPDATE)
//   - Multiple policy combination (OR for permissive, AND for restrictive)
//   - Per-role policy selection
// pgcpp implements the simplest case: a single USING qual on SELECT.
void ApplyRowSecurity(Query* query, int rt_index) {
    if (query == nullptr)
        return;
    if (rt_index < 1 || static_cast<size_t>(rt_index) > query->rtable.size())
        return;

    RangeTblEntry* rte = static_cast<RangeTblEntry*>(query->rtable[rt_index - 1]);
    if (rte == nullptr)
        return;
    if (rte->rtekind != pgcpp::parser::RTEKind::kRelation)
        return;

    // Get the stored policy qual for this relation.
    Node* policy_qual = RetrieveRowSecurityPolicy(rte->relid);
    if (policy_qual == nullptr)
        return;

    // Clone the qual so multiple query uses are independent.
    Node* qual_copy = policy_qual->Clone();

    // Add to the RTE's security_quals list.
    rte->security_quals.push_back(qual_copy);

    // Mark the query as having row security applied.
    query->has_row_security = true;
}

}  // namespace pgcpp::rewrite

// subselect.h — Subquery unfolding for the planner.
//
// Converted from PostgreSQL 15's src/backend/optimizer/plan/subselect.c.
//
// Handles SubLink nodes that appear in WHERE / target-list expressions:
//   - IN/ANY (SubLinkType::kAny): "expr IN (SELECT ...)" → unfold into a
//     semi-join (or inner join with distinct subquery) so the planner can
//     choose NestLoop/HashJoin/MergeJoin instead of evaluating the subquery
//     once per outer row.
//   - EXISTS (SubLinkType::kExists): "EXISTS (SELECT ...)" → unfold into a
//     semi-join (skeleton in MyToyDB).
//   - ALL (SubLinkType::kAll): skeleton; not yet unfolded.
//
// For MyToyDB's Task 15.15, the unfolding is simplified:
//   - IN-sublinks are converted to a join clause "outer = inner_subquery_var"
//     and the subquery's plan is wrapped in a SubqueryScanPath / SubqueryScan
//     Plan node. The subquery is planned independently (no shared ECs).
//   - EXISTS-sublinks are left as-is (skeleton).
#pragma once

#include "pgcpp/optimizer/path.hpp"
#include "pgcpp/parser/primnodes.hpp"

namespace mytoydb::optimizer {

// Forward declaration — defined in mytoydb/optimizer/planner.hpp.
struct PlannerInfo;

// pull_up_sublinks — walk the query's jointree and quals looking for
// SubLink nodes that can be unfolded into joins. Mutates the query in
// place: appends a new RangeTblEntry (subquery RTE) for each unfolded
// SubLink, appends a RangeTblRef to the jointree's fromlist, and replaces
// the SubLink in the quals with a join clause (Var = Var).
//
// Returns the number of SubLinks unfolded. A return of 0 means no change.
int pull_up_sublinks(PlannerInfo* root);

// convert_any_sublink_to_join — given a SubLink of type kAny (expr IN
// (SELECT ...)), build a join clause "outer_expr = subquery_var" and append
// the subquery as a subquery RTE to the query's range table. Returns the
// new join clause node, or nullptr if the SubLink cannot be unfolded.
//
// Side effects:
//   - Appends a RangeTblEntry with rtekind=kSubquery to query->rtable.
//   - Updates root->simple_rte_array accordingly (caller's responsibility
//     to call build_base_rel_infos afterward to refresh RelOptInfo).
mytoydb::parser::Node* convert_any_sublink_to_join(PlannerInfo* root,
                                                   mytoydb::parser::SubLink* sublink);

}  // namespace mytoydb::optimizer

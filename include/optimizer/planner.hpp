// planner.h — Planner entry point and PlannerInfo.
//
// Converted from PostgreSQL 15's src/include/optimizer/planner.h.
//
// The planner takes a parser Query tree and produces an executor Plan tree.
// PlannerInfo holds per-query optimizer state: the parse tree, base relation
// infos, and cost estimates accumulated during planning.
#pragma once

#include <vector>

#include "executor/plannodes.hpp"
#include "optimizer/path.hpp"
#include "parser/parsenodes.hpp"

namespace pgcpp::optimizer {

// PlannerInfo — per-query planner state.
//
// Mirrors PostgreSQL's PlannerInfo. For pgcpp's single-table ClickBench
// workload, this is simplified: we track base relations and their RelOptInfo,
// but skip join ordering, equivalence classes, and other advanced features.
//
// Task 15.15 adds `eq_classes` (EquivalenceClass list) and `canonical_pathkeys`
// (canonical PathKey list) for join planning, subquery unfolding, and merge
// join sort-order tracking.
struct PlannerInfo {
    pgcpp::parser::Query* parse = nullptr;      // the query being planned
    std::vector<RelOptInfo*> simple_rel_array;  // base relation infos (1-based)
    int limit_tuples = -1;                      // LIMIT count, -1 if none
    // --- P0 extensions (Task 15.3): PG-style planner state ---
    std::vector<pgcpp::parser::RangeTblEntry*> simple_rte_array;  // RTE array (1-based)
    std::vector<RelOptInfo*> join_rel_list;                       // join-relation list (skeleton)
    PlannerInfo* parent_root = nullptr;                           // parent query's PlannerInfo
    double tuple_fraction = 0.0;                                  // tuple fraction (LIMIT hint)
    bool has_recursion = false;                                   // recursive CTE?
    std::vector<pgcpp::parser::Node*> group_pathkeys;             // group pathkeys
    std::vector<pgcpp::parser::Node*> sort_pathkeys;              // sort pathkeys
    std::vector<pgcpp::parser::Node*> distinct_pathkeys;          // distinct pathkeys
    int wt_param_id = -1;                                         // window-function parameter ID
    // --- Task 15.15: equivalence classes & canonical pathkeys ---
    // All EquivalenceClass objects built from this query's join quals. Each
    // mergejoinable qual "a.x = b.y" adds a.x and b.y to a single EC, allowing
    // the optimizer to derive implied quals and detect mergejoinable clauses.
    std::vector<struct EquivalenceClass*> eq_classes;
    // Canonical PathKey objects registered during planning (deduplicated).
    std::vector<struct PathKey*> canonical_pathkeys;
};

// planner — top-level planner entry point.
//
// Takes a parser Query and returns an executor Plan tree.
// Dispatches on command_type: SELECT → subplanner, INSERT/UPDATE/DELETE →
// ModifyTable wrapping a subplanned source.
pgcpp::executor::Plan* planner(pgcpp::parser::Query* query);

// subplanner — plan a single SELECT query.
//
// Builds RelOptInfo for each base relation, generates paths, selects the
// cheapest, then layers aggregation/sort/limit/projection on top.
pgcpp::executor::Plan* subplanner(PlannerInfo* root);

// --- P0 extensions (Task 15.3): PG-style planner entry points ---
//
// These mirror PostgreSQL's planner.c entry hierarchy:
//   standard_planner → subquery_planner → grouping_planner → query_planner
//
// The existing planner() entry point continues to use subplanner() for
// ClickBench compatibility. The new entry points provide a parallel
// PG-style pipeline exercised by the new unit tests.

// standard_planner — PG-style top-level planner entry.
// Creates a PlannerInfo and delegates to subquery_planner.
pgcpp::executor::Plan* standard_planner(pgcpp::parser::Query* query);

// subquery_planner — plan a subquery (simplified: delegates to grouping_planner).
pgcpp::executor::Plan* subquery_planner(PlannerInfo* root, pgcpp::parser::Query* parse,
                                        PlannerInfo* parent_root, bool has_recursion,
                                        double tuple_fraction);

// grouping_planner — handle aggregation/sort/distinct/window (simplified).
// Delegates the base scan to query_planner, then wraps Agg/Sort on top.
pgcpp::executor::Plan* grouping_planner(PlannerInfo* root, double tuple_fraction, bool can_sort);

// query_planner — complete PG-style pipeline for the base scan:
//   query_planner_init → path generation → create_plan → set_plan_references.
// For single-table queries with aggregates/sort, the complete plan (including
// Agg/Sort layers) is built via Path objects and translated by create_plan.
pgcpp::executor::Plan* query_planner(PlannerInfo* root, pgcpp::parser::Query* parse);

}  // namespace pgcpp::optimizer

// planner.h — Planner entry point and PlannerInfo.
//
// Converted from PostgreSQL 15's src/include/optimizer/planner.h.
//
// The planner takes a parser Query tree and produces an executor Plan tree.
// PlannerInfo holds per-query optimizer state: the parse tree, base relation
// infos, and cost estimates accumulated during planning.
#pragma once

#include <vector>

#include "mytoydb/executor/plannodes.hpp"
#include "mytoydb/optimizer/path.hpp"
#include "mytoydb/parser/parsenodes.hpp"

namespace mytoydb::optimizer {

// PlannerInfo — per-query planner state.
//
// Mirrors PostgreSQL's PlannerInfo. For MyToyDB's single-table ClickBench
// workload, this is simplified: we track base relations and their RelOptInfo,
// but skip join ordering, equivalence classes, and other advanced features.
struct PlannerInfo {
    mytoydb::parser::Query* parse = nullptr;    // the query being planned
    std::vector<RelOptInfo*> simple_rel_array;  // base relation infos (1-based)
    int limit_tuples = -1;                      // LIMIT count, -1 if none
};

// planner — top-level planner entry point.
//
// Takes a parser Query and returns an executor Plan tree.
// Dispatches on command_type: SELECT → subplanner, INSERT/UPDATE/DELETE →
// ModifyTable wrapping a subplanned source.
mytoydb::executor::Plan* planner(mytoydb::parser::Query* query);

// subplanner — plan a single SELECT query.
//
// Builds RelOptInfo for each base relation, generates paths, selects the
// cheapest, then layers aggregation/sort/limit/projection on top.
mytoydb::executor::Plan* subplanner(PlannerInfo* root);

}  // namespace mytoydb::optimizer

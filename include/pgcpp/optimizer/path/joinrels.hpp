// joinrels.h — Join relation construction for the optimizer.
//
// Converted from PostgreSQL 15's src/backend/optimizer/path/joinrels.c.
//
// Builds join relations by combining base relations two at a time. For each
// pair of relations (outer, inner), the join planner:
//   1. Collects the join clauses between them (from joininfo + EC-implied).
//   2. Builds a join RelOptInfo via build_join_rel.
//   3. Calls add_paths_to_joinrel to generate NestLoop/HashJoin/MergeJoin
//      candidate paths.
//
// For MyToyDB's Task 15.15, the joinrel machinery is simplified:
//   - Only INNER joins.
//   - Linear join ordering: rel1 JOIN rel2 JOIN rel3 ...
//   - No GEQO/swarm optimization for >10 tables.
#pragma once

#include "pgcpp/optimizer/path.hpp"

namespace mytoydb::optimizer {

// Forward declaration — defined in mytoydb/optimizer/planner.hpp.
struct PlannerInfo;

// make_rels_by_clause_joins — for each pair of base relations in
// `outer_relids` joined to `inner_rel` by a join clause, build a joinrel
// and generate paths. Returns the list of constructed joinrels.
//
// This is the entry point used by the simplified planner: given the base
// relations already in simple_rel_array, build a single left-deep joinrel
// chain by joining each base rel (in RT order) onto the growing outer side.
void make_rels_by_clause_joins(PlannerInfo* root, RelOptInfo* outer_rel);

// build_joinrels_for_level — build all joinrels at a given "level" (number
// of base relations participating). Level 1 = base rels (already done);
// level 2 = 2-way joins; level 3 = 3-way joins; etc. For MyToyDB, we only
// invoke level 2 (joins two base rels at a time) — the planner doesn't
// chain joinrels into bigger joinrels yet.
void build_joinrels_for_level(PlannerInfo* root, int level);

}  // namespace mytoydb::optimizer

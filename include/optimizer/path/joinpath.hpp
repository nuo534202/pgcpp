// joinpath.h — Join path generation for the optimizer.
//
// Converted from PostgreSQL 15's src/backend/optimizer/path/joinpath.c.
//
// Given two base relations (or a base relation and a join relation) and the
// join clauses between them, generates candidate join paths:
//   - NestLoopPath: for each outer row, scan all inner rows. O(N*M).
//   - HashJoinPath: hash the inner side once, probe with outer rows. O(N+M).
//   - MergeJoinPath: sort both sides on the join key, then merge. O(N log N + M log M).
//
// For pgcpp's Task 15.15, the join path generation is simplified:
//   - Only INNER joins (no LEFT/RIGHT/FULL/SEMI join handling).
//   - No parallel-aware join paths.
//   - No parameterized inner paths (no LATERAL).
//   - MergeJoin always wraps both children in SortPath if their pathkeys
//     don't already satisfy the merge clause's ordering.
#pragma once

#include <vector>

#include "optimizer/path.hpp"
#include "optimizer/util/restrictinfo.hpp"
#include "parser/parsenodes.hpp"
#include "parser/primnodes.hpp"

namespace pgcpp::optimizer {

// Forward declaration — defined in pgcpp/optimizer/planner.hpp.
struct PlannerInfo;

// SpecialJoinInfo — describes a join's structural constraints (which rels
// are on each side, the join type, and whether it's a semi/anti join).
// Defined in util/relnode.hpp; forward-declared here to avoid an include cycle.
struct SpecialJoinInfo;

// add_paths_to_joinrel — generate candidate join paths for joining `outer_rel`
// and `inner_rel` into `joinrel`, using the join clauses in `restrictlist`.
//
// For each join method (NestLoop, HashJoin, MergeJoin), a candidate path is
// generated and added to `joinrel->pathlist`. The cheapest path is selected
// by add_path(). The caller is responsible for building `joinrel` first
// (typically via build_join_rel) and for assembling `restrictlist` from
// joininfo + EC-implied equalities.
void add_paths_to_joinrel(PlannerInfo* root, RelOptInfo* joinrel, RelOptInfo* outer_rel,
                          RelOptInfo* inner_rel, SpecialJoinInfo* sjinfo,
                          std::vector<RestrictInfo*> restrictlist);

}  // namespace pgcpp::optimizer

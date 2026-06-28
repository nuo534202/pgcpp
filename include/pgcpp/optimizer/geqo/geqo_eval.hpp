// geqo_eval.h — GEQO chromosome fitness evaluation.
//
// Converted from PostgreSQL 15's src/backend/optimizer/geqo/geqo_eval.c.
//
// Evaluates a chromosome (join-order permutation) by building the
// corresponding left-deep join tree and returning its estimated cost.
//
// PostgreSQL's geqo_eval.c actually constructs RelOptInfo/Path objects for
// each candidate, using a per-evaluation memory context that is reset
// between evaluations to bound memory growth. MyToyDB uses a two-phase
// approach for simplicity and speed:
//   - GeqoEvalFitness: a fast, allocation-free heuristic cost estimate used
//     during the GA loop (one call per candidate chromosome).
//   - GeqoBuildBestPath: constructs the real left-deep Path tree once, for
//     the winning chromosome, using the existing build_join_rel +
//     add_paths_to_joinrel machinery. The returned Path is the join tree
//     root that the planner wraps with Agg/Sort layers.
#pragma once

#include <vector>

#include "pgcpp/optimizer/geqo/geqo_main.hpp"

namespace mytoydb::optimizer {

// Forward declaration — defined in pgcpp/optimizer/planner.hpp.
struct PlannerInfo;
class Path;

}  // namespace mytoydb::optimizer

namespace mytoydb::optimizer::geqo {

// GeqoEvalFitness — compute a heuristic cost (lower is better) for the
// left-deep join order specified by `chromosome`. Does not allocate Path
// objects; uses only the base rels' estimated rows and cheapest_path costs.
// Returns kInvalidCost if any rel in the chromosome is missing.
Cost GeqoEvalFitness(PlannerInfo* root, const std::vector<Gene>& chromosome);

// GeqoBuildBestPath — construct the actual left-deep join Path tree for the
// given chromosome using build_join_rel + add_paths_to_joinrel. Returns the
// root Path of the join tree (the final joinrel's cheapest_path), or nullptr
// on failure. The caller wraps this Path with Agg/Sort/etc.
Path* GeqoBuildBestPath(PlannerInfo* root, const std::vector<Gene>& chromosome);

}  // namespace mytoydb::optimizer::geqo

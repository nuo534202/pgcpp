// geqo_main.h — GEQO genetic query optimizer: public API and core types.
//
// Converted from PostgreSQL 15's src/backend/optimizer/geqo/geqo_main.c
// and src/include/optimizer/geqo.h.
//
// GEQO (Genetic Query Optimization) finds a near-optimal join order for
// queries that join many tables, where the exhaustive dynamic-programming
// search is too expensive. A query with N base relations is encoded as a
// chromosome — a permutation of the relation indexes — and the genetic
// algorithm evolves a population of such permutations, evaluating each one's
// fitness as the cost of the corresponding left-deep join tree.
//
// PostgreSQL activates GEQO when the number of base relations exceeds
// geqo_threshold (default 12). pgcpp mirrors this threshold so that >10
// table JOINs route through the GEQO solver.
#pragma once

#include <limits>
#include <vector>

#include "pgcpp/optimizer/path.hpp"

namespace pgcpp::optimizer {

// Forward declaration — defined in pgcpp/optimizer/planner.hpp.
struct PlannerInfo;

namespace geqo {

// Gene — a single position in a chromosome. Holds a 1-based range table
// index identifying one base relation participating in the join.
using Gene = int;

// Chromosome — a candidate join order. `genes` is a permutation of the base
// relation indexes (e.g., for a 4-table join: {3,1,4,2} means join rel3 first,
// then rel1, then rel4, then rel2, in a left-deep tree). `fitness` caches the
// estimated total cost of that join order (lower is better); `evaluated`
// records whether the fitness has been computed yet.
struct Chromosome {
    std::vector<Gene> genes;
    Cost fitness = std::numeric_limits<Cost>::max();  // +inf = "unevaluated"
    bool evaluated = false;
};

// Cost sentinel for chromosomes that could not be evaluated (e.g., a base
// rel in the permutation is missing from simple_rel_array).
constexpr Cost kInvalidCost = std::numeric_limits<Cost>::max();

// GEQO tuning parameters (mirrors PostgreSQL GUC defaults).
constexpr int kGeqoThreshold = 12;               // # base rels that triggers GEQO
constexpr int kGeqoEffort = 1;                   // effort level (1=light .. 2=heavy)
constexpr int kGeqoPoolSize = 2;                 // population = pool_size * #rels
constexpr int kGeqoGenerations = 0;              // 0 = derive from #rels (PG formula)
constexpr double kGeqoSelectionBias = 2.0;       // tournament bias (1.0..2.0)
constexpr double kGeqoMutationProb = 0.05;       // per-child mutation probability
constexpr double kGeqoDefaultSelectivity = 0.1;  // equi-join selectivity heuristic

// ShouldUseGeqo — returns true if the query has enough base relations to
// justify the genetic search (>= kGeqoThreshold).
bool ShouldUseGeqo(const PlannerInfo* root);

// GeqoSolve — run the genetic algorithm and return the cheapest Path for
// the best join order found. The returned Path is the root of a left-deep
// join tree covering all base relations in root->simple_rel_array. Caller
// must ensure base RelOptInfos (with cheapest_path set) already exist.
Path* GeqoSolve(PlannerInfo* root);

}  // namespace geqo
}  // namespace pgcpp::optimizer

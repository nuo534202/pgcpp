// geqo_cx.h — GEQO crossover dispatcher.
//
// Converted from PostgreSQL 15's src/backend/optimizer/geqo/geqo_cx.c.
//
// PostgreSQL selects a crossover operator at random from {ERX, PMX, OX1, OX2,
// PX} for each child produced. Each operator produces a valid permutation
// from two parent permutations using a different gene-inheritance strategy.
// pgcpp mirrors this dispatcher so the choice of operator can be varied
// at runtime (and overridden in tests).
#pragma once

#include "pgcpp/optimizer/geqo/geqo_main.hpp"

namespace pgcpp::optimizer::geqo {

// CrossoverType — identifies the crossover operator. Matches PostgreSQL's
// geqo_cx variable values (ERX is the default and the most effective for
// TSP-like permutation problems).
enum class CrossoverType {
    kEdgeRecombination,  // ERX — uses an edge map; best for permutations.
    kPartiallyMapped,    // PMX — segment swap + repair.
    kOrder1,             // OX1 — copy a segment, fill remaining in order.
    kOrder2,             // OX2 — copy a segment to the same position.
    kPosition,           // PX — copy positions from parent 2 at random.
};

// Crossover — produce a child chromosome from two parents using the
// operator identified by `type`. The child's `evaluated` flag is cleared
// (its fitness must be recomputed). Returns true on success; false if the
// operator could not produce a valid child (in which case the caller may
// fall back to cloning parent 1).
bool Crossover(CrossoverType type, const Chromosome* mum, const Chromosome* dad, Chromosome* child);

// RandomCrossover — pick an operator uniformly at random and apply it.
// Returns the operator that was used (kEdgeRecombination if the random pick
// fails). Convenience wrapper used by the main GA loop.
CrossoverType RandomCrossover(const Chromosome* mum, const Chromosome* dad, Chromosome* child);

}  // namespace pgcpp::optimizer::geqo

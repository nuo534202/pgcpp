// geqo_erx.h — GEQO edge recombination crossover (ERX).
//
// Converted from PostgreSQL 15's src/backend/optimizer/geqo/geqo_erx.c.
//
// ERX is the default crossover for permutation problems. It builds an "edge
// map" recording, for each gene, the set of genes that appear adjacent to it
// in either parent. The child is then constructed greedily: starting from one
// parent's first gene, repeatedly extend the permutation with the unused gene
// that has the fewest remaining edges (preferring genes shared by both
// parents). This tends to preserve the adjacency structure of the parents,
// which is the property that matters for join order (adjacent joins share
// clauses).
#pragma once

#include "optimizer/geqo/geqo_main.hpp"

namespace pgcpp::optimizer::geqo {

// CrossoverERX — produce `child` from `mum` and `dad` using edge
// recombination. Returns true on success. The child is a valid permutation
// covering exactly the genes appearing in the parents (which must be the
// same set).
bool CrossoverERX(const Chromosome* mum, const Chromosome* dad, Chromosome* child);

}  // namespace pgcpp::optimizer::geqo

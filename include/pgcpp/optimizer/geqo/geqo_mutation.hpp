// geqo_mutation.h — GEQO chromosome mutation.
//
// Converted from PostgreSQL 15's src/backend/optimizer/geqo/geqo_mutation.c.
//
// Mutation introduces small random changes into a child chromosome to
// maintain genetic diversity. PostgreSQL's GEQO uses a single "swap two
// genes" mutation applied with probability geqo_mutation_prob per child.
// pgcpp follows the same design: pick two distinct positions uniformly at
// random and exchange their genes. Mutations must preserve the permutation
// invariant (every relation index appears exactly once).
#pragma once

#include "pgcpp/optimizer/geqo/geqo_main.hpp"

namespace pgcpp::optimizer::geqo {

// MutateChromosome — with probability `prob`, swap two uniformly chosen
// distinct positions in `chrom->genes`. Returns true if a mutation was
// applied, false otherwise. The mutation probability is taken from the
// supplied `prob` so callers can vary it independently of the global
// default (e.g., for testing).
bool MutateChromosome(Chromosome* chrom, double prob);

}  // namespace pgcpp::optimizer::geqo

// geqo_recombination.h — GEQO chromosome allocation and random initialization.
//
// Converted from PostgreSQL 15's src/backend/optimizer/geqo/geqo_recombination.c.
//
// Provides the helpers that allocate a chromosome for a given set of relation
// indexes and initialize it either as a random permutation (used to seed the
// initial population) or as an identity permutation (used as a parent
// template). The crossover and mutation operators live in their own files;
// this file only handles the "fresh chromosome" construction.
#pragma once

#include <vector>

#include "pgcpp/optimizer/geqo/geqo_main.hpp"

namespace mytoydb::optimizer::geqo {

// InitChromosomeRandom — fill `genes` with a random permutation of the
// supplied relation indexes. Uses the global GEQO RNG so the result is
// reproducible for a given seed.
void InitChromosomeRandom(Chromosome* chrom, const std::vector<Gene>& rel_ids);

// InitChromosomeIdentity — fill `genes` with the supplied indexes in order
// (no shuffling). Used as a deterministic "good" seed chromosome.
void InitChromosomeIdentity(Chromosome* chrom, const std::vector<Gene>& rel_ids);

// AllocateChromosome — create a new Chromosome with `genes` left empty.
// Returned pointer is allocated via palloc in the current memory context.
Chromosome* AllocateChromosome();

}  // namespace mytoydb::optimizer::geqo

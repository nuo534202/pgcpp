// geqo_recombination.cpp — GEQO chromosome allocation and random init.
//
// Converted from PostgreSQL 15's src/backend/optimizer/geqo/geqo_recombination.c.
//
// Implements the random-permutation initializer (Fisher-Yates shuffle) used
// to seed the initial population, and the identity initializer used to inject
// one "natural order" chromosome into the pool (matching PostgreSQL's
// behavior of always including the RT-order join as a candidate).
#include "pgcpp/optimizer/geqo/geqo_recombination.hpp"

#include <algorithm>

#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/optimizer/geqo/geqo_random.hpp"

namespace pgcpp::optimizer::geqo {
using pgcpp::nodes::makePallocNode;

Chromosome* AllocateChromosome() {
    return makePallocNode<Chromosome>();
}

void InitChromosomeRandom(Chromosome* chrom, const std::vector<Gene>& rel_ids) {
    if (chrom == nullptr)
        return;
    chrom->genes = rel_ids;
    chrom->evaluated = false;
    chrom->fitness = kInvalidCost;
    if (chrom->genes.size() < 2)
        return;
    // Fisher-Yates shuffle using the global GEQO RNG.
    for (size_t i = chrom->genes.size(); i > 1; --i) {
        int j = GeqoRng().NextInt(static_cast<int>(i));
        std::swap(chrom->genes[i - 1], chrom->genes[static_cast<size_t>(j)]);
    }
}

void InitChromosomeIdentity(Chromosome* chrom, const std::vector<Gene>& rel_ids) {
    if (chrom == nullptr)
        return;
    chrom->genes = rel_ids;
    chrom->evaluated = false;
    chrom->fitness = kInvalidCost;
}

}  // namespace pgcpp::optimizer::geqo

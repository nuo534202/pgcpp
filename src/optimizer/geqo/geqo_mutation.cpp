// geqo_mutation.cpp — GEQO chromosome mutation.
//
// Converted from PostgreSQL 15's src/backend/optimizer/geqo/geqo_mutation.c.
//
// Implements swap mutation: with probability `prob`, pick two distinct
// positions in the chromosome at uniform random and exchange their genes.
// This is the simplest mutation operator that preserves the permutation
// invariant (each relation index appears exactly once).
#include "optimizer/geqo/geqo_mutation.hpp"

#include <algorithm>

#include "optimizer/geqo/geqo_random.hpp"

namespace pgcpp::optimizer::geqo {

bool MutateChromosome(Chromosome* chrom, double prob) {
    if (chrom == nullptr || chrom->genes.size() < 2)
        return false;
    if (GeqoRng().NextDouble() >= prob)
        return false;
    int n = static_cast<int>(chrom->genes.size());
    int i = GeqoRng().NextInt(n);
    int j = GeqoRng().NextInt(n - 1);
    if (j >= i)
        ++j;  // ensure j != i
    std::swap(chrom->genes[static_cast<size_t>(i)], chrom->genes[static_cast<size_t>(j)]);
    chrom->evaluated = false;  // gene order changed → fitness stale
    chrom->fitness = kInvalidCost;
    return true;
}

}  // namespace pgcpp::optimizer::geqo

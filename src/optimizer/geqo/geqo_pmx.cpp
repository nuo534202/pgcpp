// geqo_pmx.cpp — GEQO partially mapped crossover (PMX).
//
// Converted from PostgreSQL 15's src/backend/optimizer/geqo/geqo_pmx.c.
//
// PMX (standard formulation):
//   1. Pick two cut points i, j (i <= j) uniformly at random.
//   2. Copy mum[i..j] into the child at the same positions.
//   3. Build a mapping mum[k] -> dad[k] for k in [i, j].
//   4. For each position k NOT in [i, j]: take g = dad[k]; if g is already
//      in the segment (i.e., g equals some mum[k'] for k' in [i,j]), follow
//      the mapping chain g = map[g] until g is no longer a segment gene;
//      place g at k.
//
// The mapping mum[k] -> dad[k] is correct (not dad[k] -> mum[k]) because
// outside positions take dad's genes: if dad[k] = mum[k'] (already placed
// in the segment), we replace it with dad[k'] (the gene dad would have
// placed at the conflicting position). The chain terminates because dad is
// a permutation: dad[k'] is unique, and the chain visits distinct dad genes
// in the segment until one is not a mum gene in the segment.
#include "optimizer/geqo/geqo_pmx.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "optimizer/geqo/geqo_random.hpp"

namespace pgcpp::optimizer::geqo {

bool CrossoverPMX(const Chromosome* mum, const Chromosome* dad, Chromosome* child) {
    if (mum == nullptr || dad == nullptr || child == nullptr)
        return false;
    const size_t n = mum->genes.size();
    if (n == 0 || dad->genes.size() != n)
        return false;

    if (n == 1) {
        child->genes = mum->genes;
        child->evaluated = false;
        child->fitness = kInvalidCost;
        return true;
    }

    int a = GeqoRng().NextInt(static_cast<int>(n));
    int b = GeqoRng().NextInt(static_cast<int>(n));
    int i = std::min(a, b);
    int j = std::max(a, b);

    child->genes.assign(n, 0);
    child->evaluated = false;
    child->fitness = kInvalidCost;

    // Set of mum genes placed in the segment (the "already used" set).
    std::unordered_set<Gene> segment_genes;
    // Mapping mum[k] -> dad[k] for k in [i, j]. When an outside dad gene
    // equals a mum gene in the segment, we replace it with the
    // corresponding dad gene from that position.
    std::unordered_map<Gene, Gene> mum_to_dad;

    for (int k = i; k <= j; ++k) {
        Gene mg = mum->genes[static_cast<size_t>(k)];
        Gene dg = dad->genes[static_cast<size_t>(k)];
        child->genes[static_cast<size_t>(k)] = mg;
        segment_genes.insert(mg);
        mum_to_dad[mg] = dg;
    }

    // For positions outside [i, j], copy dad's gene, following the mapping
    // chain if the gene is already in the segment.
    for (size_t k = 0; k < n; ++k) {
        if (k >= static_cast<size_t>(i) && k <= static_cast<size_t>(j))
            continue;
        Gene g = dad->genes[k];
        // Follow the mapping chain while g is a mum gene in the segment.
        while (segment_genes.find(g) != segment_genes.end()) {
            auto it = mum_to_dad.find(g);
            if (it == mum_to_dad.end())
                break;  // g is in segment but not a mapped mum gene — stop.
            g = it->second;
        }
        child->genes[k] = g;
    }

    return true;
}

}  // namespace pgcpp::optimizer::geqo

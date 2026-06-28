// geqo_px.cpp — GEQO position crossover (PX).
//
// Converted from PostgreSQL 15's src/backend/optimizer/geqo/geqo_px.c.
//
// PX:
//   1. For each position k, with 50% probability, copy dad[k] into the child
//      at position k (marking it placed).
//   2. Fill remaining empty positions with mum's genes in order, skipping any
//      gene already placed from dad.
// This preserves the "positions" of roughly half of parent 2's genes and
// fills the rest with parent 1's ordering.
#include "pgcpp/optimizer/geqo/geqo_px.hpp"

#include <unordered_set>

#include "pgcpp/optimizer/geqo/geqo_random.hpp"

namespace pgcpp::optimizer::geqo {

bool CrossoverPX(const Chromosome* mum, const Chromosome* dad, Chromosome* child) {
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

    child->genes.assign(n, 0);
    child->evaluated = false;
    child->fitness = kInvalidCost;

    std::unordered_set<Gene> placed;

    // Step 1: with 50% probability per position, take dad's gene.
    for (size_t k = 0; k < n; ++k) {
        if (GeqoRng().NextDouble() < 0.5) {
            Gene g = dad->genes[k];
            if (placed.find(g) == placed.end()) {
                child->genes[k] = g;
                placed.insert(g);
            }
        }
    }

    // Step 2: fill remaining empty positions with mum's genes in order.
    for (size_t k = 0; k < n; ++k) {
        if (child->genes[k] != 0)
            continue;  // already filled from dad
        for (size_t m = 0; m < n; ++m) {
            Gene g = mum->genes[m];
            if (placed.find(g) == placed.end()) {
                child->genes[k] = g;
                placed.insert(g);
                break;
            }
        }
    }
    return true;
}

}  // namespace pgcpp::optimizer::geqo

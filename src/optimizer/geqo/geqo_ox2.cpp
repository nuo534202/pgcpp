// geqo_ox2.cpp — GEQO order crossover #2 (OX2).
//
// Converted from PostgreSQL 15's src/backend/optimizer/geqo/geqo_ox2.c.
//
// OX2:
//   1. Pick two cut points i, j (i <= j) uniformly at random.
//   2. Copy mum[i..j] into the child at the same positions.
//   3. Fill remaining positions starting from position 0 with dad's genes in
//      the order they appear in dad, skipping any gene already present.
// The fill order starts from position 0 (no wrapping), unlike OX1.
#include "pgcpp/optimizer/geqo/geqo_ox2.hpp"

#include <algorithm>
#include <unordered_set>

#include "pgcpp/optimizer/geqo/geqo_random.hpp"

namespace pgcpp::optimizer::geqo {

bool CrossoverOX2(const Chromosome* mum, const Chromosome* dad, Chromosome* child) {
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

    std::unordered_set<Gene> placed;
    for (int k = i; k <= j; ++k) {
        child->genes[static_cast<size_t>(k)] = mum->genes[static_cast<size_t>(k)];
        placed.insert(mum->genes[static_cast<size_t>(k)]);
    }

    // Fill remaining positions starting from position 0 (no wrapping).
    size_t fill_pos = 0;
    for (size_t k = 0; k < n; ++k) {
        // Skip the segment.
        if (k >= static_cast<size_t>(i) && k <= static_cast<size_t>(j))
            continue;
        // Find the next unused gene from dad.
        while (fill_pos < n) {
            Gene g = dad->genes[fill_pos];
            ++fill_pos;
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

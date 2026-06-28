// geqo_ox1.cpp — GEQO order crossover #1 (OX1).
//
// Converted from PostgreSQL 15's src/backend/optimizer/geqo/geqo_ox1.c.
//
// OX1:
//   1. Pick two cut points i, j (i <= j) uniformly at random.
//   2. Copy mum[i..j] into the child at the same positions.
//   3. Starting from position (j+1) mod n, fill the remaining empty positions
//      with dad's genes in the order they appear in dad, wrapping around and
//      skipping any gene already present in the child.
// This preserves the relative order of dad's genes outside the segment.
#include "pgcpp/optimizer/geqo/geqo_ox1.hpp"

#include <algorithm>
#include <unordered_set>

#include "pgcpp/optimizer/geqo/geqo_random.hpp"

namespace mytoydb::optimizer::geqo {

bool CrossoverOX1(const Chromosome* mum, const Chromosome* dad, Chromosome* child) {
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

    // Fill remaining positions starting after j, wrapping around.
    size_t fill_pos = static_cast<size_t>((j + 1) % static_cast<int>(n));
    for (size_t step = 0; step < n; ++step) {
        size_t pos = (fill_pos + step) % n;
        if (pos >= static_cast<size_t>(i) && pos <= static_cast<size_t>(j))
            continue;  // segment already filled
        // Find the next unused gene from dad (starting from dad's beginning).
        for (size_t k = 0; k < n; ++k) {
            Gene g = dad->genes[k];
            if (placed.find(g) == placed.end()) {
                child->genes[pos] = g;
                placed.insert(g);
                break;
            }
        }
    }
    return true;
}

}  // namespace mytoydb::optimizer::geqo

// geqo_misc.cpp — GEQO miscellaneous helpers.
//
// Converted from PostgreSQL 15's src/backend/optimizer/geqo/geqo_misc.c.
//
// Implements the small utilities used by geqo_main.cpp: counting base rels,
// collecting their RT indexes, validating permutation invariants, and
// extracting the fittest chromosome from a population.
#include "optimizer/geqo/geqo_misc.hpp"

#include <algorithm>
#include <unordered_set>

#include "optimizer/planner.hpp"

namespace pgcpp::optimizer::geqo {

int CountBaseRels(const PlannerInfo* root) {
    if (root == nullptr)
        return 0;
    int count = 0;
    for (const RelOptInfo* rel : root->simple_rel_array) {
        if (rel != nullptr)
            ++count;
    }
    return count;
}

std::vector<Gene> CollectBaseRelIds(const PlannerInfo* root) {
    std::vector<Gene> ids;
    if (root == nullptr)
        return ids;
    // simple_rel_array is conceptually 1-based: slot [0] holds relid 1.
    // Preserve RT order so chromosomes are deterministic for a given query.
    for (size_t i = 0; i < root->simple_rel_array.size(); ++i) {
        if (root->simple_rel_array[i] != nullptr)
            ids.push_back(static_cast<Gene>(i + 1));
    }
    return ids;
}

bool IsValidPermutation(const Chromosome& chrom, const std::vector<Gene>& expected) {
    if (chrom.genes.size() != expected.size())
        return false;
    std::unordered_set<Gene> seen;
    seen.reserve(expected.size());
    for (Gene g : chrom.genes) {
        if (!seen.insert(g).second)
            return false;  // duplicate gene
    }
    // Verify that every expected gene appears in the chromosome.
    for (Gene g : expected) {
        if (seen.find(g) == seen.end())
            return false;
    }
    return true;
}

Chromosome* FindBestChromosome(std::vector<Chromosome*>& pool) {
    Chromosome* best = nullptr;
    Cost best_cost = kInvalidCost;
    for (Chromosome* c : pool) {
        if (c == nullptr || !c->evaluated)
            continue;
        if (best == nullptr || c->fitness < best_cost) {
            best = c;
            best_cost = c->fitness;
        }
    }
    // If nothing was evaluated, fall back to the first non-null entry so
    // the caller still gets *something* (it will be evaluated later).
    if (best == nullptr) {
        for (Chromosome* c : pool) {
            if (c != nullptr) {
                best = c;
                break;
            }
        }
    }
    return best;
}

}  // namespace pgcpp::optimizer::geqo

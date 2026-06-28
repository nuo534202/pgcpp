// geqo_erx.cpp — GEQO edge recombination crossover (ERX).
//
// Converted from PostgreSQL 15's src/backend/optimizer/geqo/geqo_erx.c.
//
// ERX builds an edge map (each gene's neighbors across both parents) and
// constructs the child by repeatedly selecting the unused neighbor with the
// smallest edge count, breaking ties in favor of genes that appear as
// neighbors in both parents. The result is a valid permutation that
// preserves parental adjacency — useful for join ordering, where adjacent
// relations share join clauses.
#include "pgcpp/optimizer/geqo/geqo_erx.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "pgcpp/optimizer/geqo/geqo_random.hpp"

namespace pgcpp::optimizer::geqo {

namespace {

// Add `b` as a neighbor of `a` in `edges` (bidirectional insertion is done
// by the caller for both directions). Uses std::unordered_set for O(1)
// membership tests and deduplication.
inline void AddEdge(std::unordered_map<Gene, std::unordered_set<Gene>>* edges, Gene a, Gene b) {
    (*edges)[a].insert(b);
}

// Count how many of `gene`'s neighbors are still unused.
int CountUnusedNeighbors(const std::unordered_map<Gene, std::unordered_set<Gene>>& edges, Gene gene,
                         const std::unordered_set<Gene>& used) {
    auto it = edges.find(gene);
    if (it == edges.end())
        return 0;
    int count = 0;
    for (Gene n : it->second) {
        if (used.find(n) == used.end())
            ++count;
    }
    return count;
}

}  // namespace

bool CrossoverERX(const Chromosome* mum, const Chromosome* dad, Chromosome* child) {
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

    // Build the edge map: for each gene, the set of genes adjacent to it in
    // either parent. Two parents contribute up to 2 edges per gene (left and
    // right neighbors), but duplicates collapse in the set.
    std::unordered_map<Gene, std::unordered_set<Gene>> edges;
    for (size_t i = 0; i < n; ++i) {
        Gene a = mum->genes[i];
        Gene b = mum->genes[(i + 1) % n];
        if (a != b) {
            AddEdge(&edges, a, b);
            AddEdge(&edges, b, a);
        }
        Gene c = dad->genes[i];
        Gene d = dad->genes[(i + 1) % n];
        if (c != d) {
            AddEdge(&edges, c, d);
            AddEdge(&edges, d, c);
        }
    }

    std::unordered_set<Gene> used;
    child->genes.clear();
    child->genes.reserve(n);

    // Start from mum's first gene (matches PostgreSQL's choice).
    Gene current = mum->genes[0];
    child->genes.push_back(current);
    used.insert(current);

    for (size_t step = 1; step < n; ++step) {
        // Candidates = unused neighbors of `current`.
        std::vector<Gene> candidates;
        auto it = edges.find(current);
        if (it != edges.end()) {
            for (Gene n : it->second) {
                if (used.find(n) == used.end())
                    candidates.push_back(n);
            }
        }
        if (candidates.empty()) {
            // No unused neighbor — fall back to any unused gene.
            for (size_t k = 0; k < n; ++k) {
                Gene g = mum->genes[k];
                if (used.find(g) == used.end())
                    candidates.push_back(g);
            }
        }
        if (candidates.empty())
            return false;  // shouldn't happen if inputs are valid permutations

        // Pick the candidate with the fewest remaining unused neighbors,
        // breaking ties at random. This is the ERX heuristic.
        Gene best = candidates[0];
        int best_count = CountUnusedNeighbors(edges, best, used);
        for (size_t i = 1; i < candidates.size(); ++i) {
            int c = CountUnusedNeighbors(edges, candidates[i], used);
            if (c < best_count || (c == best_count && GeqoRng().NextDouble() < 0.5)) {
                best = candidates[i];
                best_count = c;
            }
        }
        child->genes.push_back(best);
        used.insert(best);
        current = best;
    }

    child->evaluated = false;
    child->fitness = kInvalidCost;
    return true;
}

}  // namespace pgcpp::optimizer::geqo

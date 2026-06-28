// geqo_main.cpp — GEQO main driver: threshold check and GA loop.
//
// Converted from PostgreSQL 15's src/backend/optimizer/geqo/geqo_main.c.
//
// Implements the two public entry points declared in geqo_main.hpp:
//   - ShouldUseGeqo: true if the query has >= kGeqoThreshold base rels.
//   - GeqoSolve: run the genetic algorithm and return the cheapest Path
//     for the best join order found.
//
// The GA loop mirrors PostgreSQL's structure:
//   1. Collect base relation indexes (the gene pool).
//   2. Compute population size and generation count (ComputeGeqoParams).
//   3. Seed the global GEQO RNG (deterministic per query).
//   4. Initialize the population: half random permutations, half identity
//      (giving the GA both diversity and a sensible starting point).
//   5. Evaluate the initial population via GeqoEvalFitness.
//   6. For each generation: select two parents via tournament selection,
//      produce a child via random crossover, mutate it, evaluate it, and
//      replace the worst population member if the child is better.
//   7. Build the real left-deep Path tree for the winning chromosome via
//      GeqoBuildBestPath and return it.
//
// The chromosome fitness is a fast heuristic (NestLoop-style cost); the
// actual Path tree is built only once, for the winner. This two-phase
// strategy avoids the per-evaluation memory growth that PG pays for with
// a per-evaluation memory context reset.
#include "pgcpp/optimizer/geqo/geqo_main.hpp"

#include <algorithm>
#include <vector>

#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/optimizer/geqo/geqo_copy.hpp"
#include "pgcpp/optimizer/geqo/geqo_cx.hpp"
#include "pgcpp/optimizer/geqo/geqo_eval.hpp"
#include "pgcpp/optimizer/geqo/geqo_misc.hpp"
#include "pgcpp/optimizer/geqo/geqo_mutation.hpp"
#include "pgcpp/optimizer/geqo/geqo_params.hpp"
#include "pgcpp/optimizer/geqo/geqo_random.hpp"
#include "pgcpp/optimizer/geqo/geqo_recombination.hpp"
#include "pgcpp/optimizer/planner.hpp"
#include "pgcpp/optimizer/util/relnode.hpp"

namespace mytoydb::optimizer::geqo {
using mytoydb::nodes::makePallocNode;

namespace {

// TournamentSelection — pick `bias`-weighted better of two random members.
// Returns the index of the selected chromosome in `pool`. PostgreSQL uses a
// linear-bias tournament: with probability 1/bias the loser of the previous
// round is reconsidered, yielding a bias toward fitter members.
size_t TournamentSelection(std::vector<Chromosome*>& pool, double bias) {
    int n = static_cast<int>(pool.size());
    if (n <= 0)
        return 0;
    size_t a = static_cast<size_t>(GeqoRng().NextInt(n));
    size_t b = static_cast<size_t>(GeqoRng().NextInt(n));
    while (b == a && n > 1)
        b = static_cast<size_t>(GeqoRng().NextInt(n));
    Chromosome* ca = pool[a];
    Chromosome* cb = pool[b];
    // Lower fitness is better. If both unevaluated, prefer the first.
    bool a_better = (ca != nullptr && ca->evaluated) &&
                    (!(cb != nullptr && cb->evaluated) || ca->fitness <= cb->fitness);
    if (a_better) {
        // With probability (1 - 1/bias), keep the winner; otherwise flip.
        if (GeqoRng().NextDouble() < 1.0 / bias)
            return b;
        return a;
    }
    if (GeqoRng().NextDouble() < 1.0 / bias)
        return a;
    return b;
}

// FindWorstIndex — return the index of the chromosome with the highest
// (worst) fitness in `pool`. Used to identify the slot to replace.
size_t FindWorstIndex(std::vector<Chromosome*>& pool) {
    size_t worst = 0;
    Cost worst_cost = std::numeric_limits<Cost>::lowest();
    for (size_t i = 0; i < pool.size(); ++i) {
        Chromosome* c = pool[i];
        if (c == nullptr)
            return i;  // null slot is always the worst
        Cost f = c->evaluated ? c->fitness : kInvalidCost;
        if (f > worst_cost) {
            worst_cost = f;
            worst = i;
        }
    }
    return worst;
}

}  // namespace

bool ShouldUseGeqo(const PlannerInfo* root) {
    return CountBaseRels(root) >= kGeqoThreshold;
}

Path* GeqoSolve(PlannerInfo* root) {
    if (root == nullptr)
        return nullptr;
    // 1. Collect base relation indexes — the gene pool for chromosomes.
    std::vector<Gene> rel_ids = CollectBaseRelIds(root);
    if (rel_ids.empty())
        return nullptr;
    if (rel_ids.size() == 1) {
        // Single-table query: no join order to optimize, just return the
        // base rel's cheapest path (mirrors PG's behavior for trivial cases).
        RelOptInfo* rel = find_base_rel(root, rel_ids[0]);
        return (rel != nullptr) ? rel->cheapest_path : nullptr;
    }

    // 2. Compute GA parameters from the relation count.
    GeqoParams params = ComputeGeqoParams(static_cast<int>(rel_ids.size()));
    int pool_size = params.pool_size;
    int generations = params.generations;

    // 3. Seed the global RNG deterministically (PG uses the PID + a counter;
    // MyToyDB uses a fixed seed for reproducibility across runs, keyed on
    // the relation count so different queries get different orderings).
    SetGeqoSeed(static_cast<uint64_t>(rel_ids.size()) * 0x9E3779B97F4A7C15ULL ^
                0x123456789ABCDEF0ULL);

    // 4. Initialize the population: half random permutations, half identity.
    // PostgreSQL always seeds with the identity chromosome so the "natural"
    // RT-order join is a candidate; the rest are random for diversity.
    std::vector<Chromosome*> pool;
    pool.reserve(static_cast<size_t>(pool_size));
    for (int i = 0; i < pool_size; ++i) {
        Chromosome* c = AllocateChromosome();
        if (i == 0) {
            InitChromosomeIdentity(c, rel_ids);
        } else {
            InitChromosomeRandom(c, rel_ids);
        }
        pool.push_back(c);
    }

    // 5. Evaluate the initial population.
    for (Chromosome* c : pool) {
        if (c == nullptr)
            continue;
        c->fitness = GeqoEvalFitness(root, c->genes);
        c->evaluated = true;
    }

    // 6. GA loop: select, crossover, mutate, evaluate, replace worst.
    Chromosome child_storage;  // reused across generations to avoid realloc.
    for (int gen = 0; gen < generations; ++gen) {
        size_t mum_idx = TournamentSelection(pool, params.selection_bias);
        size_t dad_idx = TournamentSelection(pool, params.selection_bias);
        while (dad_idx == mum_idx && pool.size() > 1)
            dad_idx = TournamentSelection(pool, params.selection_bias);

        Chromosome* mum = pool[mum_idx];
        Chromosome* dad = pool[dad_idx];
        if (mum == nullptr || dad == nullptr)
            continue;

        // Crossover produces a fresh child; reset the storage's stale state.
        child_storage.genes.clear();
        child_storage.evaluated = false;
        child_storage.fitness = kInvalidCost;
        CrossoverType cx = RandomCrossover(mum, dad, &child_storage);

        // If crossover failed (e.g., degenerate inputs), clone mum.
        if (child_storage.genes.empty()) {
            CopyChromosome(&child_storage, mum);
        }
        (void)cx;  // operator used is recorded for tracing but not exposed.

        // Mutate (with low probability) to maintain diversity.
        MutateChromosome(&child_storage, params.mutation_prob);

        // Evaluate the child.
        child_storage.fitness = GeqoEvalFitness(root, child_storage.genes);
        child_storage.evaluated = true;

        // Replace the worst population member if the child is better.
        size_t worst_idx = FindWorstIndex(pool);
        Chromosome* worst = (worst_idx < pool.size()) ? pool[worst_idx] : nullptr;
        if (worst == nullptr || child_storage.fitness < worst->fitness) {
            // The replaced Chromosome is owned by the memory context; we
            // allocate a fresh one to hold the child so the pool keeps
            // stable pointers (no dangling references for selection).
            Chromosome* new_member = AllocateChromosome();
            CopyChromosome(new_member, &child_storage);
            pool[worst_idx] = new_member;
        }
    }

    // 7. Extract the best chromosome and build the real Path tree.
    Chromosome* best = FindBestChromosome(pool);
    if (best == nullptr)
        return nullptr;
    // If for some reason the winner was never evaluated, evaluate it now.
    if (!best->evaluated) {
        best->fitness = GeqoEvalFitness(root, best->genes);
        best->evaluated = true;
    }
    return GeqoBuildBestPath(root, best->genes);
}

}  // namespace mytoydb::optimizer::geqo

// geqo_params.cpp — GEQO population/generation sizing.
//
// Converted from PostgreSQL 15's src/backend/optimizer/geqo/geqo_params.c.
//
// Derives the population size and generation count from the relation count
// and the geqo_effort parameter. Larger joins get proportionally larger
// populations; the number of generations is bounded so that very large
// joins don't spend too long in the GA loop.
#include "pgcpp/optimizer/geqo/geqo_params.hpp"

#include <algorithm>

namespace mytoydb::optimizer::geqo {

GeqoParams ComputeGeqoParams(int num_rels) {
    GeqoParams p;
    if (num_rels < 2)
        num_rels = 2;
    // PostgreSQL: pool_size = effort * num_rels * kGeqoPoolSize (rounded).
    int pool = kGeqoEffort * num_rels * kGeqoPoolSize;
    if (pool < 2)
        pool = 2;
    p.pool_size = pool;

    // PostgreSQL: generations = (num_rels <= pool) ? pool : pool / num_rels.
    // When geqo_generations is 0 (auto), use the formula; otherwise honor it.
    if (kGeqoGenerations > 0) {
        p.generations = kGeqoGenerations;
    } else if (num_rels <= pool) {
        p.generations = pool;
    } else {
        p.generations = std::max(1, pool / num_rels);
    }

    p.mutation_prob = kGeqoMutationProb;
    p.selection_bias = kGeqoSelectionBias;
    return p;
}

}  // namespace mytoydb::optimizer::geqo

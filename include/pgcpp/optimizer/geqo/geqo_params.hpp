// geqo_params.h — GEQO population/generation sizing.
//
// Converted from PostgreSQL 15's src/backend/optimizer/geqo/geqo_params.c.
//
// PostgreSQL derives the genetic algorithm's population size and number of
// generations from the number of relations being joined, scaled by the
// geqo_effort GUC. MyToyDB mirrors this so that larger joins get larger
// populations (more thorough search) without an unbounded blow-up.
#pragma once

#include "pgcpp/optimizer/geqo/geqo_main.hpp"

namespace mytoydb::optimizer::geqo {

// GeqoParams — sizing parameters for one genetic-algorithm run.
struct GeqoParams {
    int pool_size = 0;    // number of chromosomes in the population
    int generations = 0;  // number of GA generations to evolve
    double mutation_prob = kGeqoMutationProb;
    double selection_bias = kGeqoSelectionBias;
};

// ComputeGeqoParams — derive population size and generation count for a join
// of `num_rels` base relations. PostgreSQL's formula (geqo_params.c):
//   pool_size  = effort * num_rels * kGeqoPoolSize
//   generations = (num_rels <= pool_size) ? pool_size : pool_size / num_rels
// The minimum pool size is 2 so that even tiny joins still evolve.
GeqoParams ComputeGeqoParams(int num_rels);

}  // namespace mytoydb::optimizer::geqo

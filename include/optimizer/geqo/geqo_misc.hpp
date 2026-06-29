// geqo_misc.h — GEQO miscellaneous helpers.
//
// Converted from PostgreSQL 15's src/backend/optimizer/geqo/geqo_misc.c.
//
// Provides utility functions shared across the GEQO module:
//   - CountBaseRels: count non-null slots in simple_rel_array (used by the
//     threshold check and to size the chromosome).
//   - CollectBaseRelIds: gather the 1-based RT indexes of all base rels in
//     RT order; this becomes the gene pool from which chromosomes permute.
//   - IsValidPermutation: sanity-check that a chromosome is a permutation of
//     the expected gene set (every rel appears exactly once).
//   - FindBestChromosome: scan a population and return a pointer to the
//     chromosome with the lowest fitness (used to extract the winner).
#pragma once

#include <vector>

#include "optimizer/geqo/geqo_main.hpp"

namespace pgcpp::optimizer {

struct PlannerInfo;  // forward declaration

namespace geqo {

// CountBaseRels — return the number of non-null entries in
// root->simple_rel_array (i.e., the number of base relations participating
// in the query). Used by ShouldUseGeqo() to decide whether to engage GEQO.
int CountBaseRels(const PlannerInfo* root);

// CollectBaseRelIds — return the 1-based RT indexes of all base relations
// in root->simple_rel_array, in ascending order. The returned vector is
// the gene pool from which chromosomes are permuted.
std::vector<Gene> CollectBaseRelIds(const PlannerInfo* root);

// IsValidPermutation — return true if `chrom` contains exactly the genes in
// `expected` (each appearing exactly once), i.e., it is a valid permutation
// of the expected gene set. Used as a debug assertion inside the GA loop.
bool IsValidPermutation(const Chromosome& chrom, const std::vector<Gene>& expected);

// FindBestChromosome — return a pointer to the chromosome in `pool` with
// the lowest evaluated fitness. Chromosomes that have not been evaluated
// are skipped. Returns nullptr if the pool is empty or no chromosome has
// been evaluated.
Chromosome* FindBestChromosome(std::vector<Chromosome*>& pool);

}  // namespace geqo
}  // namespace pgcpp::optimizer

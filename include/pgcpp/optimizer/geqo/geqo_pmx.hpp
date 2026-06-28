// geqo_pmx.h — GEQO partially mapped crossover (PMX).
//
// Converted from PostgreSQL 15's src/backend/optimizer/geqo/geqo_pmx.c.
//
// PMX copies a contiguous segment from parent 1 into the child, then fills
// the remaining positions using parent 2's genes — but when a parent 2 gene
// is already present in the copied segment, it is replaced by the gene that
// was "mapped" out of that position (using the segment as a partial mapping).
// This preserves the permutation invariant while mixing parental material.
#pragma once

#include "pgcpp/optimizer/geqo/geqo_main.hpp"

namespace mytoydb::optimizer::geqo {

bool CrossoverPMX(const Chromosome* mum, const Chromosome* dad, Chromosome* child);

}  // namespace mytoydb::optimizer::geqo

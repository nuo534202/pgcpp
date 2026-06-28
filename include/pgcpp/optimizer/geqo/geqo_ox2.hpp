// geqo_ox2.h — GEQO order crossover #2 (OX2).
//
// Converted from PostgreSQL 15's src/backend/optimizer/geqo/geqo_ox2.c.
//
// OX2 copies a contiguous segment from parent 1 into the child at the same
// positions, then fills the remaining positions starting from position 0
// with parent 2's genes in the order they appear in parent 2 (skipping genes
// already placed). Unlike OX1, the fill does not wrap — it starts from the
// beginning of the array.
#pragma once

#include "pgcpp/optimizer/geqo/geqo_main.hpp"

namespace pgcpp::optimizer::geqo {

bool CrossoverOX2(const Chromosome* mum, const Chromosome* dad, Chromosome* child);

}  // namespace pgcpp::optimizer::geqo

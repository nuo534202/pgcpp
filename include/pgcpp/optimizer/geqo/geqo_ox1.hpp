// geqo_ox1.h — GEQO order crossover #1 (OX1).
//
// Converted from PostgreSQL 15's src/backend/optimizer/geqo/geqo_ox1.c.
//
// OX1 copies a contiguous segment from parent 1 into the child, then fills
// the remaining positions with parent 2's genes in the order they appear,
// starting just after the copied segment and wrapping around. This preserves
// the relative ordering of parent 2's genes outside the segment.
#pragma once

#include "pgcpp/optimizer/geqo/geqo_main.hpp"

namespace pgcpp::optimizer::geqo {

bool CrossoverOX1(const Chromosome* mum, const Chromosome* dad, Chromosome* child);

}  // namespace pgcpp::optimizer::geqo

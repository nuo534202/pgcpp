// geqo_px.h — GEQO position crossover (PX).
//
// Converted from PostgreSQL 15's src/backend/optimizer/geqo/geqo_px.c.
//
// PX iterates over each position; with 50% probability it copies the gene
// from parent 2 into the child at that position. After all positions are
// considered, the remaining empty slots are filled with parent 1's genes in
// order (skipping those already placed). This preserves the "positions" of
// some of parent 2's genes while taking the rest from parent 1.
#pragma once

#include "pgcpp/optimizer/geqo/geqo_main.hpp"

namespace pgcpp::optimizer::geqo {

bool CrossoverPX(const Chromosome* mum, const Chromosome* dad, Chromosome* child);

}  // namespace pgcpp::optimizer::geqo

// geqo_copy.h — GEQO chromosome copy.
//
// Converted from PostgreSQL 15's src/backend/optimizer/geqo/geqo_copy.c.
//
// Provides a deep-copy helper for chromosomes. PostgreSQL needs this because
// its chromosomes are allocated as bare Gene arrays and must be copied before
// mutation; pgcpp uses std::vector<Gene>, so the copy is trivial, but the
// helper is kept to mirror the PG file layout and to centralize any future
// deep-copy semantics (e.g., attached path trees).
#pragma once

#include "pgcpp/optimizer/geqo/geqo_main.hpp"

namespace pgcpp::optimizer::geqo {

// CopyChromosome — deep-copy `src` into `dst`. The fitness cache is copied
// as well, so the destination does not need to be re-evaluated.
void CopyChromosome(Chromosome* dst, const Chromosome* src);

}  // namespace pgcpp::optimizer::geqo

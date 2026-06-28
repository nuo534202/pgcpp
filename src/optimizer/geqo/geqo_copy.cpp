// geqo_copy.cpp — GEQO chromosome copy.
//
// Converted from PostgreSQL 15's src/backend/optimizer/geqo/geqo_copy.c.
//
// Deep-copies a Chromosome. Because pgcpp stores genes in a std::vector,
// the copy is a straightforward member-wise assignment; the helper exists to
// keep the PG file layout and to provide a single point where future
// non-trivial deep-copy semantics (e.g., cached path trees) can be added.
#include "pgcpp/optimizer/geqo/geqo_copy.hpp"

namespace pgcpp::optimizer::geqo {

void CopyChromosome(Chromosome* dst, const Chromosome* src) {
    if (dst == nullptr || src == nullptr)
        return;
    dst->genes = src->genes;
    dst->fitness = src->fitness;
    dst->evaluated = src->evaluated;
}

}  // namespace pgcpp::optimizer::geqo

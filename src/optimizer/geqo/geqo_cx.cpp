// geqo_cx.cpp — GEQO crossover dispatcher.
//
// Converted from PostgreSQL 15's src/backend/optimizer/geqo/geqo_cx.c.
//
// Dispatches to one of the five crossover operators (ERX, PMX, OX1, OX2, PX)
// based on the caller-supplied type, or selects one uniformly at random.
// Each operator is implemented in its own translation unit (geqo_erx.cpp,
// geqo_pmx.cpp, geqo_ox1.cpp, geqo_ox2.cpp, geqo_px.cpp) to mirror the PG
// file layout.
#include "pgcpp/optimizer/geqo/geqo_cx.hpp"

#include "pgcpp/optimizer/geqo/geqo_erx.hpp"
#include "pgcpp/optimizer/geqo/geqo_ox1.hpp"
#include "pgcpp/optimizer/geqo/geqo_ox2.hpp"
#include "pgcpp/optimizer/geqo/geqo_pmx.hpp"
#include "pgcpp/optimizer/geqo/geqo_px.hpp"
#include "pgcpp/optimizer/geqo/geqo_random.hpp"

namespace mytoydb::optimizer::geqo {

bool Crossover(CrossoverType type, const Chromosome* mum, const Chromosome* dad,
               Chromosome* child) {
    switch (type) {
        case CrossoverType::kEdgeRecombination:
            return CrossoverERX(mum, dad, child);
        case CrossoverType::kPartiallyMapped:
            return CrossoverPMX(mum, dad, child);
        case CrossoverType::kOrder1:
            return CrossoverOX1(mum, dad, child);
        case CrossoverType::kOrder2:
            return CrossoverOX2(mum, dad, child);
        case CrossoverType::kPosition:
            return CrossoverPX(mum, dad, child);
    }
    return false;
}

CrossoverType RandomCrossover(const Chromosome* mum, const Chromosome* dad, Chromosome* child) {
    // Pick one of the five operators uniformly at random. ERX is slightly
    // favored in PostgreSQL; MyToyDB keeps the uniform selection for
    // simplicity and deterministic behavior under a fixed seed.
    int pick = GeqoRng().NextInt(5);
    CrossoverType type = CrossoverType::kEdgeRecombination;
    switch (pick) {
        case 0:
            type = CrossoverType::kEdgeRecombination;
            break;
        case 1:
            type = CrossoverType::kPartiallyMapped;
            break;
        case 2:
            type = CrossoverType::kOrder1;
            break;
        case 3:
            type = CrossoverType::kOrder2;
            break;
        case 4:
            type = CrossoverType::kPosition;
            break;
        default:
            break;
    }
    bool ok = Crossover(type, mum, dad, child);
    if (!ok) {
        // Fall back to ERX (the most robust permutation crossover); if even
        // ERX fails (e.g., degenerate inputs), the caller will clone a parent.
        type = CrossoverType::kEdgeRecombination;
        Crossover(type, mum, dad, child);
    }
    return type;
}

}  // namespace mytoydb::optimizer::geqo

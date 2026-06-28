// geqo_io.cpp — GEQO chromosome debug I/O.
//
// Converted from PostgreSQL 15's src/backend/optimizer/geqo/geqo_io.c.
//
// Implements FormatChromosome, which serializes a chromosome for trace
// output. The format mirrors PostgreSQL's:
//   "{g1,g2,...,gN} (cost=NNN.NN)"
// where the cost is shown only when the chromosome has been evaluated.
#include "pgcpp/optimizer/geqo/geqo_io.hpp"

#include <sstream>

namespace pgcpp::optimizer::geqo {

std::string FormatChromosome(const Chromosome& chrom) {
    std::ostringstream os;
    os << "{";
    for (size_t i = 0; i < chrom.genes.size(); ++i) {
        if (i > 0)
            os << ",";
        os << chrom.genes[i];
    }
    os << "}";
    if (chrom.evaluated) {
        // Show fitness with two decimals (PG prints COST fixed-point).
        os << " (cost=" << chrom.fitness << ")";
    } else {
        os << " (cost=?)";
    }
    return os.str();
}

}  // namespace pgcpp::optimizer::geqo

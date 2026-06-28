// geqo_io.h — GEQO chromosome debug I/O.
//
// Converted from PostgreSQL 15's src/backend/optimizer/geqo/geqo_io.c.
//
// Provides human-readable formatting of a chromosome for debugging and
// trace output. PostgreSQL's geqo_io.c implements both a printer and a
// parser (used by geqo_pool_print and the debug trace hooks). MyToyDB keeps
// only the printer: the parser exists for interactive GA tuning, which is
// not exposed here. The format is a comma-separated list of gene values
// enclosed in braces, e.g. "{3,1,4,2}".
#pragma once

#include <string>

#include "pgcpp/optimizer/geqo/geqo_main.hpp"

namespace mytoydb::optimizer::geqo {

// FormatChromosome — return a human-readable representation of `chrom` in
// the form "{g1,g2,...,gN}" followed by the cached fitness in parentheses.
// Intended for debug logging and unit-test diagnostics.
std::string FormatChromosome(const Chromosome& chrom);

}  // namespace mytoydb::optimizer::geqo

#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "pgcpp/types/ts_types.hpp"

namespace pgcpp::tsearch {

// ---------------------------------------------------------------------------
// ts_typanalyze (PostgreSQL src/backend/tsearch/ts_typanalyze.c).
//
// Computes statistics over a column of tsvectors to help the planner pick a
// good selectivity estimate. We track the most-common lexemes and the
// average number of entries per vector.
// ---------------------------------------------------------------------------

struct TsVectorStats {
    // Most-common lexemes sorted by descending frequency.
    struct MceEntry {
        std::string lexeme;
        std::size_t count;
    };
    std::vector<MceEntry> most_common_lexemes;
    double average_length = 0.0;  // average number of entries per vector
    std::size_t sample_count = 0;
};

// Analyze a list of tsvector samples. Returns the computed statistics.
TsVectorStats TsVectorAnalyze(const std::vector<const pgcpp::types::TsVectorData*>& samples);

}  // namespace pgcpp::tsearch

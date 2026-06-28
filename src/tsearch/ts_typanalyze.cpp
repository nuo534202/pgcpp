// ts_typanalyze.cpp — collect statistics over a column of tsvectors.
//
// For each sample vector, count the occurrences of every lexeme across the
// entire sample set. The most common lexemes (by total count) are returned,
// along with the average number of entries per vector.

#include "pgcpp/tsearch/ts_typanalyze.hpp"

#include <algorithm>
#include <map>

namespace mytoydb::tsearch {

using mytoydb::types::TsVectorData;

TsVectorStats TsVectorAnalyze(const std::vector<const mytoydb::types::TsVectorData*>& samples) {
    TsVectorStats stats;
    stats.sample_count = samples.size();
    if (samples.empty()) {
        return stats;
    }
    std::map<std::string, std::size_t> counts;
    std::size_t total_entries = 0;
    for (const TsVectorData* vec : samples) {
        if (vec == nullptr) {
            continue;
        }
        total_entries += vec->entries.size();
        for (const auto& entry : vec->entries) {
            ++counts[entry.lexeme];
        }
    }
    stats.average_length = static_cast<double>(total_entries) / static_cast<double>(samples.size());
    // Sort by descending count, then by lexeme for stability.
    std::vector<TsVectorStats::MceEntry> mce;
    mce.reserve(counts.size());
    for (auto& [lexeme, count] : counts) {
        mce.push_back({lexeme, count});
    }
    std::sort(mce.begin(), mce.end(),
              [](const TsVectorStats::MceEntry& a, const TsVectorStats::MceEntry& b) {
                  if (a.count != b.count) {
                      return a.count > b.count;
                  }
                  return a.lexeme < b.lexeme;
              });
    // Keep the top-10 most common lexemes.
    if (mce.size() > 10) {
        mce.resize(10);
    }
    stats.most_common_lexemes = std::move(mce);
    return stats;
}

}  // namespace mytoydb::tsearch

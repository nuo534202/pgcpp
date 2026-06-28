#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "pgcpp/types/ts_types.hpp"

namespace mytoydb::tsearch {

// ---------------------------------------------------------------------------
// Full-text search utility functions (PostgreSQL utils/adt/tsrank.c,
// ts_headline.c, tsquery_rewrite.c).
//
// All functions operate on the in-memory TsVectorData / TsQueryData types
// defined in mytoydb/types/ts_types.hpp.
// ---------------------------------------------------------------------------

// ts_rank — standard ranking based on the frequency of matched lexemes and
// their weights. Higher score means a better match. Returns 0.0 for a query
// that does not match the document at all.
float ts_rank(const mytoydb::types::TsVectorData& vec, const mytoydb::types::TsQueryData& query);

// ts_rank_cd — cover density ranking. Rewards matches that are close
// together in the document. Simplified to score based on the position
// proximity of matched lexemes.
float ts_rank_cd(const mytoydb::types::TsVectorData& vec, const mytoydb::types::TsQueryData& query);

// ts_headline — extract a snippet of `text` around the first matched lexeme
// in `query`. Wraps the matched word with `start_sel`..`stop_sel` markers
// (defaults: <b> .. </b>). `max_words` limits the snippet length.
std::string ts_headline(std::string_view text, const mytoydb::types::TsQueryData& query,
                        std::size_t max_words = 35, std::string_view start_sel = "<b>",
                        std::string_view stop_sel = "</b>");

// ts_rewrite — given a query and a vector of (target, replacement) rules,
// substitute every target subtree with its replacement. Each rule replaces a
// single lexeme with another query tree.
struct RewriteRule {
    std::string target_lexeme;
    mytoydb::types::TsQueryNode replacement;
};

mytoydb::types::TsQueryNode ts_rewrite(const mytoydb::types::TsQueryNode& query,
                                       const std::vector<RewriteRule>& rules);

}  // namespace mytoydb::tsearch

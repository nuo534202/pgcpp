#pragma once

#include <string>
#include <string_view>

#include "types/ts_types.hpp"

namespace pgcpp::tsearch {

// ---------------------------------------------------------------------------
// to_tsvector — main entry point (PostgreSQL to_tsvector(text)).
//
// Pipeline: tokenize → apply dictionary (lowercase + simple stemming +
// stop-word removal) → build TsVectorData.
//
// `config` selects the dictionary configuration. Supported values:
//   "simple" — lowercase only, no stop-word removal.
//   "english" (default) — lowercase + simple stemming + stop-word removal.
// ---------------------------------------------------------------------------

pgcpp::types::TsVectorData ToTsVector(std::string_view text, std::string_view config = "english");

}  // namespace pgcpp::tsearch

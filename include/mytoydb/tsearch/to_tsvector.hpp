#pragma once

#include <string>
#include <string_view>

#include "mytoydb/types/ts_types.hpp"

namespace mytoydb::tsearch {

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

mytoydb::types::TsVectorData ToTsVector(std::string_view text, std::string_view config = "english");

}  // namespace mytoydb::tsearch

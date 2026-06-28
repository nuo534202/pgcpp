#pragma once

#include <string>
#include <string_view>

#include "mytoydb/types/ts_types.hpp"

namespace mytoydb::tsearch {

// ---------------------------------------------------------------------------
// to_tsquery — main entry point (PostgreSQL to_tsquery(text)).
//
// Pipeline: tokenize → apply dictionary → build a TsQueryData that ANDs
// every surviving lexeme together.
//
// `config` selects the dictionary configuration (same options as ToTsVector).
// ---------------------------------------------------------------------------

mytoydb::types::TsQueryData ToTsQuery(std::string_view text, std::string_view config = "english");

}  // namespace mytoydb::tsearch

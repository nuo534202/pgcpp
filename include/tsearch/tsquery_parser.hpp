#pragma once

#include <string>
#include <string_view>

#include "types/ts_types.hpp"

namespace pgcpp::tsearch {

// ---------------------------------------------------------------------------
// tsquery parser (PostgreSQL src/backend/tsearch/tsquery_parser.c).
//
// Parses a tsquery string literal supporting the operators:
//   &   AND
//   |   OR
//   !   NOT (unary prefix)
//   ( ) grouping
//
// Returns the root TsQueryNode tree. Reuses pgcpp::types::TsQueryNode so
// the resulting tree is interoperable with the existing types::ts_match and
// types::tsquery_out routines.
//
// Precedence (highest to lowest): ! , & , |. This matches PostgreSQL.
// ---------------------------------------------------------------------------

pgcpp::types::TsQueryNode TsQueryParse(std::string_view str);

}  // namespace pgcpp::tsearch

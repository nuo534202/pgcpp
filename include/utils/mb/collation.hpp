// collation.hpp — collation-aware string comparison.
//
// Converted from PostgreSQL 15's src/backend/utils/adt/varlena.c
// (varstr_cmp and related functions) and src/backend/utils/mb/wchar.c.
//
// PostgreSQL supports three collation providers:
//   - 'd' (default): uses the database encoding's byte comparison
//   - 'c' (libc): uses strcoll() / memcmp() depending on the locale
//   - 'i' (icu): uses ICU's ucol_strcollUTF8()
//
// pgcpp does not link against ICU. Instead, it implements:
//   - C/POSIX collation: byte-by-byte comparison (memcmp)
//   - Default collation: byte-by-byte comparison
//   - ICU-style collation: Unicode codepoint comparison (for UTF-8, this
//     means comparing decoded codepoints; for single-byte encodings,
//     comparing bytes directly)
//
// This gives correct results for ASCII and for codepoint-ordered Unicode
// (which is the default for many use cases). Locale-specific sorting
// (e.g. German ä < a, Swedish å < z) is not supported without ICU.

#pragma once

#include <cstdint>
#include <string_view>

#include "catalog/catalog.hpp"
#include "catalog/pg_collation.hpp"
#include "utils/mb/encoding.hpp"

namespace pgcpp::utils {

// VarStrCmp — compare two strings using the given collation.
// Returns < 0 if a < b, 0 if a == b, > 0 if a > b.
// If collation_oid is kInvalidOid or refers to the "default"/"C"/"POSIX"
// collation, a byte-by-byte comparison is used.
// For ICU collations, a Unicode codepoint comparison is used.
int VarStrCmp(std::string_view a, std::string_view b, pgcpp::catalog::Oid collation_oid);

// VarStrCmpC — byte-by-byte comparison (C/POSIX collation).
int VarStrCmpC(std::string_view a, std::string_view b);

// VarStrCmpCodepoint — Unicode codepoint comparison.
// For UTF-8 strings, decodes to codepoints and compares.
// For non-UTF-8 strings, falls back to byte comparison.
int VarStrCmpCodepoint(std::string_view a, std::string_view b, PgEncoding enc);

// GetCollationProvider — look up a collation's provider from the catalog.
// Returns the provider, or kDefault if the collation is not found.
pgcpp::catalog::CollProvider GetCollationProvider(pgcpp::catalog::Oid collation_oid);

}  // namespace pgcpp::utils

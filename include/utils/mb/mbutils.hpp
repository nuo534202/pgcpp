// mbutils.hpp — high-level multibyte encoding utilities.
//
// Converted from PostgreSQL 15's src/backend/utils/mb/mbutils.c.
//
// Provides database encoding management (get/set the server encoding),
// client/server encoding conversion, and convenience functions for
// checking whether a string is valid in the current encoding.

#pragma once

#include <string>
#include <string_view>

#include "utils/mb/encoding.hpp"

namespace pgcpp::utils {

// --- Database encoding management ---

// GetDatabaseEncoding — return the current server encoding.
// Defaults to UTF8 until SetDatabaseEncoding is called.
PgEncoding GetDatabaseEncoding();

// SetDatabaseEncoding — set the server encoding (called during database
// creation or startup).
void SetDatabaseEncoding(PgEncoding enc);

// GetDatabaseEncodingName — return the name of the current server encoding.
const char* GetDatabaseEncodingName();

// --- Client encoding management ---

// GetClientEncoding — return the current client encoding.
// Defaults to the server encoding.
PgEncoding GetClientEncoding();

// SetClientEncoding — set the client encoding for the current session.
void SetClientEncoding(PgEncoding enc);

// --- Encoding conversion ---

// PgConvert — convert a string from one encoding to another.
// Returns the converted string. If conversion is not supported (or not
// needed because the encodings match), returns the input unchanged.
std::string PgConvert(PgEncoding src_enc, PgEncoding dest_enc, std::string_view src);

// PgConvertTo — convert from the current database encoding to the
// specified encoding.
std::string PgConvertTo(PgEncoding dest_enc, std::string_view src);

// PgConvertFrom — convert from the specified encoding to the current
// database encoding.
std::string PgConvertFrom(PgEncoding src_enc, std::string_view src);

// --- Validation ---

// PgVerifyMbstr — check that `str` is valid in the given encoding.
// Returns true if valid, false otherwise.
bool PgVerifyMbstr(PgEncoding enc, std::string_view str);

// PgValidServerEncoding — same as IsValidServerEncoding (re-exported for
// PG API compatibility).
bool PgValidServerEncoding(PgEncoding enc);

}  // namespace pgcpp::utils

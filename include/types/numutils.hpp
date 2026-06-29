#pragma once

#include <cstdint>

namespace pgcpp::types {

// pg_strtoint64 — PostgreSQL-style int64 parsing with strict validation.
// Raises ereport(ERROR) on invalid syntax or out-of-range values.
int64_t pg_strtoint64(const char* str);

// float8_out_internal — shortest-round-trip decimal output for doubles.
// Matches PostgreSQL's float8_out with extra_float_digits=1: emits the
// shortest decimal representation that round-trips through strtod.
// Returns a palloc'd C string. Special values map to "Infinity",
// "-Infinity", and "NaN" (PostgreSQL conventions).
char* float8_out_internal(double val);

}  // namespace pgcpp::types

#pragma once

#include <cstdint>

#include "pgcpp/types/datum.hpp"

namespace pgcpp::types {

// NumericData — a simplified arbitrary-precision decimal type.
//
// Stores a scaled integer: actual_value = value / 10^dscale.
// The sign is implicit in `value` (int128 supports negative values).
// This mirrors PostgreSQL's numeric semantics closely enough for
// ClickBench AVG/SUM aggregation, while keeping the implementation
// compact (the full PG numeric algorithm is 1000+ lines).
//
// Datum storage: palloc'd NumericData; the Datum is a pointer.
struct NumericData {
    __int128 value;  // scaled integer
    int32_t dscale;  // number of decimal digits after the point
};

// --- I/O ---
// Parse a decimal string ("123", "123.456", "-123.456") into a NumericData.
// Raises ereport(ERROR) on invalid syntax.
Datum numeric_in(const char* str);
// Format a NumericData Datum as a PostgreSQL-style decimal string.
char* numeric_out(Datum value);

// --- construction ---
// Build a NumericData datum from a scaled integer and dscale.
Datum MakeNumericDatum(__int128 value, int32_t dscale);
// Construct a numeric from a 64-bit signed integer (dscale = 0).
Datum Int64ToNumeric(int64_t val);
// Construct a numeric from a 32-bit signed integer (dscale = 0).
Datum Int32ToNumeric(int32_t val);
// Construct a numeric from a double (uses up to 15 fractional digits).
Datum Float8ToNumeric(double val);

// --- arithmetic ---
Datum numeric_add(Datum a, Datum b);
Datum numeric_sub(Datum a, Datum b);
Datum numeric_mul(Datum a, Datum b);
Datum numeric_div(Datum a, Datum b);

// --- comparison ---
// Returns -1, 0, 1 like PostgreSQL's numeric_cmp.
int numeric_cmp(Datum a, Datum b);
Datum numeric_eq(Datum a, Datum b);
Datum numeric_lt(Datum a, Datum b);

// --- aggregation helpers ---
// Accumulate `newval` into the running sum `trans`. Returns the new sum.
Datum numeric_accum(Datum trans, Datum newval);
// Compute AVG = sum / count. `sum` is a numeric Datum, `count` is int64.
Datum numeric_avg(Datum sum, int64_t count);

// --- conversions ---
int64_t numeric_to_int64(Datum value);
double numeric_to_float8(Datum value);

// --- internal accessors (used by tests) ---
// Dereference a numeric Datum as a NumericData pointer.
inline NumericData* DatumGetNumeric(Datum x) {
    return reinterpret_cast<NumericData*>(x);
}

}  // namespace pgcpp::types

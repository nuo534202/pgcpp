#pragma once

#include <cstdint>

#include "types/datum.hpp"

namespace pgcpp::types {

// Base-10,000 digit array representation (mirrors PostgreSQL's NBASE).
// Each int16_t holds 4 decimal digits, value in [0, 9999].
constexpr int kNumericBase = 10000;
constexpr int kNumericPos = 0;
constexpr int kNumericNeg = 1;

// NumericData — base-10,000 variable-length arbitrary-precision decimal.
//
// Mirrors PostgreSQL's utils/adt/numeric.c NumericVar layout:
//   value = sign * Σ(digits[i] * NBASE^(weight - i))  for i in [0, ndigits)
//
// Semantics:
//   - digits[0] is the most significant stored group; weight names its
//     position (0 = ones group, -1 = first fractional group, 1 = NBASEs).
//   - dscale is the display scale (decimal digits after the point); it
//     determines how many fractional digits numeric_out renders.
//   - Trailing zero digits beyond what dscale requires are stripped.
//   - Leading zero digits are stripped; if all digits are zero the value
//     is positive zero with weight 0.
//
// Datum storage: palloc'd NumericData; the Datum is a pointer. The digits
// array is a separate palloc allocation owned by the NumericData.
struct NumericData {
    int ndigits;      // number of int16_t digit slots in use
    int weight;       // position of digits[0] (0 = ones, -1 = first frac)
    int sign;         // kNumericPos or kNumericNeg
    int dscale;       // display scale (decimal digits after the point)
    int16_t* digits;  // palloc'd array of ndigits int16_t, each [0, NBASE-1]
};

// --- I/O ---
// Parse a decimal string ("123", "123.456", "-123.456") into a NumericData.
// Raises ereport(ERROR) on invalid syntax.
Datum numeric_in(const char* str);
// Format a NumericData Datum as a PostgreSQL-style decimal string.
char* numeric_out(Datum value);

// --- construction ---
// Build a NumericData datum from a base-10000 digit array.
// `digits` may be nullptr when ndigits == 0 (yields canonical zero).
Datum MakeNumericDatum(const int16_t* digits, int ndigits, int weight, int sign, int dscale);
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

// --- rounding / truncation ---
// Round to `new_dscale` decimal digits, half away from zero.
// If new_dscale >= current dscale, the result is zero-padded (no rounding).
Datum numeric_round(Datum value, int32_t new_dscale);
// Truncate toward zero to `new_dscale` decimal digits.
Datum numeric_trunc(Datum value, int32_t new_dscale);
// Smallest integer >= value (round toward +inf).
Datum numeric_ceil(Datum value);
// Largest integer <= value (round toward -inf).
Datum numeric_floor(Datum value);
// Absolute value: |x|. Preserves dscale.
Datum numeric_abs(Datum value);

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

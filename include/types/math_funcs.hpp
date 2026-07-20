// math_funcs.hpp — built-in mathematical SQL functions.
//
// Declares the Datum-by-value entry points for PostgreSQL's math builtins
// (abs, ceil, floor, round, sqrt, power, log, log10, exp, mod, sign, trunc).
// abs(int4), round, ceil, floor, sqrt, power, mod(int4,int4) delegate to
// the existing implementations in builtins.cpp; this header covers the
// remaining overloads (abs(int8), abs(float8), log, log10, exp, sign,
// trunc with one or two arguments) plus thin wrappers re-exported under
// pgcpp::types for convenient fmgr dispatch.
#pragma once

#include "types/datum.hpp"

namespace pgcpp::types {

// abs(int8) → int8
Datum int8_abs(Datum a);
// abs(float8) → float8 (delegates to the existing float8_abs implementation)
Datum float8_abs_d(Datum a);

// log(float8) → float8 — natural logarithm (ln).
Datum float8_ln(Datum a);
// log10(float8) → float8 — base-10 logarithm.
Datum float8_log10(Datum a);
// exp(float8) → float8 — e^x.
Datum float8_exp(Datum a);

// trunc(float8, int4) → float8 — truncate to N decimal places.
// N = 0 truncates to integer; negative N truncates digits left of the
// decimal point.
Datum float8_trunc_n(Datum a, Datum n);

// mod(int8, int8) → int8 — bigint remainder (mirrors int4_mod).
Datum int8_mod(Datum a, Datum b);

// sign(int4) → int4 — returns -1, 0, or 1.
Datum int4_sign(Datum a);

}  // namespace pgcpp::types

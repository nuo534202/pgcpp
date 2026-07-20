// math_funcs.cpp — built-in mathematical SQL functions.
//
// Implements the math builtins declared in math_funcs.hpp. abs(int4),
// abs(int8), mod(int4), mod(int8), float8_abs, float8_ceil/floor/round/
// trunc/sign are already provided by builtins.cpp; this file adds the
// natural / base-10 logarithm, exp, sign(int4) [re-exported here as the
// catalog entry name "sign"], trunc(float8, int), and a thin re-export
// of float8_abs under a distinct symbol. Together these cover the 11
// math functions required by Task 9: abs, ceil, floor, round, sqrt,
// power, log, exp, mod, sign, trunc (with log10 added as a base-10
// alias).

#include "types/math_funcs.hpp"

#include <cmath>

#include "common/error/elog.hpp"
#include "types/builtins.hpp"

namespace pgcpp::types {

using pgcpp::error::LogLevel;

// abs(float8) — re-exported under the math_funcs TU for symmetry with
// the int8 overload; the actual computation lives in builtins.cpp.
Datum float8_abs_d(Datum a) {
    return float8_abs(a);
}

// log(float8) — natural logarithm (PostgreSQL's ln).
Datum float8_ln(Datum a) {
    double v = DatumGetFloat8(a);
    if (v < 0.0) {
        ereport(LogLevel::kError, "cannot take logarithm of a negative number");
    }
    if (v == 0.0) {
        ereport(LogLevel::kError, "cannot take logarithm of zero");
    }
    return Float8GetDatum(std::log(v));
}

// log10(float8) — base-10 logarithm.
Datum float8_log10(Datum a) {
    double v = DatumGetFloat8(a);
    if (v < 0.0) {
        ereport(LogLevel::kError, "cannot take logarithm of a negative number");
    }
    if (v == 0.0) {
        ereport(LogLevel::kError, "cannot take logarithm of zero");
    }
    return Float8GetDatum(std::log10(v));
}

// exp(float8) — e^x.
Datum float8_exp(Datum a) {
    return Float8GetDatum(std::exp(DatumGetFloat8(a)));
}

// trunc(float8, int4) — truncate to N decimal places.
// PostgreSQL semantics: positive N keeps N fractional digits, negative N
// zeros out digits left of the decimal point, N = 0 truncates to integer.
Datum float8_trunc_n(Datum a, Datum n) {
    double v = DatumGetFloat8(a);
    int32_t digits = DatumGetInt32(n);
    double scale = 1.0;
    if (digits >= 0) {
        for (int i = 0; i < digits; ++i) {
            scale *= 10.0;
        }
    } else {
        for (int i = 0; i < -digits; ++i) {
            scale *= 10.0;
        }
        // Truncate digits left of the decimal point.
        double scaled = v / scale;
        double trunc_scaled = std::trunc(scaled);
        return Float8GetDatum(trunc_scaled * scale);
    }
    double scaled = v * scale;
    double trunc_scaled = std::trunc(scaled);
    return Float8GetDatum(trunc_scaled / scale);
}

// sign(int4) — returns -1, 0, or 1.
Datum int4_sign(Datum a) {
    int32_t v = DatumGetInt32(a);
    if (v > 0) {
        return Int32GetDatum(1);
    }
    if (v < 0) {
        return Int32GetDatum(-1);
    }
    return Int32GetDatum(0);
}

}  // namespace pgcpp::types

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "pgcpp/types/datum.hpp"

namespace pgcpp::types {

// bool input/output
Datum bool_in(const char* str);
char* bool_out(Datum value);

// int2 input/output (SMALLINT)
Datum int2_in(const char* str);
char* int2_out(Datum value);

// int4 input/output
Datum int4_in(const char* str);
char* int4_out(Datum value);

// int8 input/output
Datum int8_in(const char* str);
char* int8_out(Datum value);

// float8 input/output
Datum float8_in(const char* str);
char* float8_out(Datum value);

// text input/output
Datum text_in(const char* str);
char* text_out(Datum value);

// varchar input/output (same as text for now)
Datum varchar_in(const char* str);
char* varchar_out(Datum value);

// Comparison functions (return -1, 0, 1)
int int2_cmp(Datum a, Datum b);
int int4_cmp(Datum a, Datum b);
int int8_cmp(Datum a, Datum b);
int float8_cmp(Datum a, Datum b);
int text_cmp(Datum a, Datum b);

// --- Comparison operators (return bool Datum) ---
// bool
Datum bool_eq(Datum a, Datum b);
Datum bool_ne(Datum a, Datum b);
Datum bool_lt(Datum a, Datum b);
Datum bool_le(Datum a, Datum b);
Datum bool_gt(Datum a, Datum b);
Datum bool_ge(Datum a, Datum b);

// int2
Datum int2_eq(Datum a, Datum b);
Datum int2_ne(Datum a, Datum b);
Datum int2_lt(Datum a, Datum b);
Datum int2_le(Datum a, Datum b);
Datum int2_gt(Datum a, Datum b);
Datum int2_ge(Datum a, Datum b);

// int4
Datum int4_eq(Datum a, Datum b);
Datum int4_ne(Datum a, Datum b);
Datum int4_lt(Datum a, Datum b);
Datum int4_le(Datum a, Datum b);
Datum int4_gt(Datum a, Datum b);
Datum int4_ge(Datum a, Datum b);

// int8
Datum int8_eq(Datum a, Datum b);
Datum int8_ne(Datum a, Datum b);
Datum int8_lt(Datum a, Datum b);
Datum int8_le(Datum a, Datum b);
Datum int8_gt(Datum a, Datum b);
Datum int8_ge(Datum a, Datum b);

// float8
Datum float8_eq(Datum a, Datum b);
Datum float8_ne(Datum a, Datum b);
Datum float8_lt(Datum a, Datum b);
Datum float8_le(Datum a, Datum b);
Datum float8_gt(Datum a, Datum b);
Datum float8_ge(Datum a, Datum b);

// text
Datum text_eq(Datum a, Datum b);
Datum text_ne(Datum a, Datum b);
Datum text_lt(Datum a, Datum b);
Datum text_le(Datum a, Datum b);
Datum text_gt(Datum a, Datum b);
Datum text_ge(Datum a, Datum b);

// --- Arithmetic ---
// int2
Datum int2_pl(Datum a, Datum b);   // a + b
Datum int2_mi(Datum a, Datum b);   // a - b
Datum int2_mul(Datum a, Datum b);  // a * b
Datum int2_div(Datum a, Datum b);  // a / b

// int4
Datum int4_pl(Datum a, Datum b);   // a + b
Datum int4_mi(Datum a, Datum b);   // a - b
Datum int4_mul(Datum a, Datum b);  // a * b
Datum int4_div(Datum a, Datum b);  // a / b
Datum int4_mod(Datum a, Datum b);  // a % b
Datum int4_um(Datum a);            // unary minus
Datum int4_abs(Datum a);           // absolute value
Datum int4_inc(Datum a);           // a + 1

// int8
Datum int8_pl(Datum a, Datum b);
Datum int8_mi(Datum a, Datum b);
Datum int8_mul(Datum a, Datum b);
Datum int8_div(Datum a, Datum b);
Datum int8_mod(Datum a, Datum b);
Datum int8_um(Datum a);
Datum int8_abs(Datum a);
Datum int8_inc(Datum a);

// float8
Datum float8_pl(Datum a, Datum b);
Datum float8_mi(Datum a, Datum b);
Datum float8_mul(Datum a, Datum b);
Datum float8_div(Datum a, Datum b);
Datum float8_um(Datum a);
Datum float8_abs(Datum a);
Datum float8_ceil(Datum a);
Datum float8_floor(Datum a);
Datum float8_round(Datum a);
Datum float8_trunc(Datum a);
Datum float8_sign(Datum a);

// --- Type conversions ---
Datum i2toi4(Datum a);  // int2 -> int4
Datum i4toi2(Datum a);  // int4 -> int2 (range-checked)

// text concatenation
Datum text_concat(Datum a, Datum b);

// Helper: create a text Datum from a std::string_view.
// Allocates via palloc in the current memory context.
Datum MakeTextDatum(std::string_view str);

// Helper: extract a std::string from a text Datum.
std::string TextDatumToString(Datum datum);

}  // namespace pgcpp::types

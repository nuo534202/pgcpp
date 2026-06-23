#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "mytoydb/types/datum.h"

namespace mytoydb::types {

// bool input/output
Datum bool_in(const char* str);
char* bool_out(Datum value);

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
int int4_cmp(Datum a, Datum b);
int int8_cmp(Datum a, Datum b);
int float8_cmp(Datum a, Datum b);
int text_cmp(Datum a, Datum b);

// Arithmetic for int4
Datum int4_pl(Datum a, Datum b);   // a + b
Datum int4_mi(Datum a, Datum b);   // a - b
Datum int4_mul(Datum a, Datum b);  // a * b
Datum int4_div(Datum a, Datum b);  // a / b

// Arithmetic for float8
Datum float8_pl(Datum a, Datum b);
Datum float8_mi(Datum a, Datum b);
Datum float8_mul(Datum a, Datum b);
Datum float8_div(Datum a, Datum b);

// text concatenation
Datum text_concat(Datum a, Datum b);

// Helper: create a text Datum from a std::string_view.
// Allocates via palloc in the current memory context.
Datum MakeTextDatum(std::string_view str);

// Helper: extract a std::string from a text Datum.
std::string TextDatumToString(Datum datum);

}  // namespace mytoydb::types

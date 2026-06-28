#pragma once

#include <cstdint>

#include "pgcpp/types/datum.hpp"

namespace pgcpp::types {

// ---------------------------------------------------------------------------
// bit / varbit — fixed and variable length bit strings.
//
// bit(n)    (OID 1560): exactly n bits, padded with trailing zeros on input.
// varbit(n) (OID 1562): at most n bits.
//
// Storage: a palloc'd VarBit struct. The header (int32 length) is followed
// by `(bitlen + 7) / 8` bytes of bit data, MSB-first.
// ---------------------------------------------------------------------------

constexpr uint32_t kVarBitHeaderSize = 4;

struct VarBit {
    int32_t bit_len;  // number of valid bits
    uint8_t bits[1];  // flexible array
};

Datum bit_in(const char* str, int32_t typmod);     // bit(n): typmod = n
Datum varbit_in(const char* str, int32_t typmod);  // varbit(n): typmod = n
char* bit_out(Datum value);
char* varbit_out(Datum value);

// Convenience forms without typmod (length inferred from input).
Datum bit_in_default(const char* str);
Datum varbit_in_default(const char* str);

int varbit_cmp(Datum a, Datum b);
Datum varbit_eq(Datum a, Datum b);
Datum varbit_ne(Datum a, Datum b);
Datum varbit_lt(Datum a, Datum b);
Datum varbit_le(Datum a, Datum b);
Datum varbit_gt(Datum a, Datum b);
Datum varbit_ge(Datum a, Datum b);

// Length in bits.
Datum bit_length(Datum value);

// bit-and / bit-or / bit-xor / bit-not
Datum bit_and(Datum a, Datum b);
Datum bit_or(Datum a, Datum b);
Datum bit_xor(Datum a, Datum b);
Datum bit_not(Datum a);

// Helpers.
Datum MakeVarBitDatum(const uint8_t* bits, int32_t bit_len);
inline VarBit* DatumGetVarBit(Datum x) {
    return reinterpret_cast<VarBit*>(x);
}

}  // namespace pgcpp::types

#pragma once

#include <cstdint>
#include <cstring>

#include "types/datum.hpp"

namespace pgcpp::types {

// ---------------------------------------------------------------------------
// UUID — 128-bit identifier (PostgreSQL type OID 2950).
//
// Display form (PostgreSQL convention):
//   XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX
// (32 hex digits in five groups separated by hyphens; case-insensitive input,
// lowercase output).
//
// Storage: a palloc'd UuidData; the Datum is a pointer.
// ---------------------------------------------------------------------------

struct UuidData {
    uint8_t bytes[16];
};

Datum uuid_in(const char* str);
char* uuid_out(Datum value);

int uuid_cmp(Datum a, Datum b);
Datum uuid_eq(Datum a, Datum b);
Datum uuid_ne(Datum a, Datum b);
Datum uuid_lt(Datum a, Datum b);
Datum uuid_le(Datum a, Datum b);
Datum uuid_gt(Datum a, Datum b);
Datum uuid_ge(Datum a, Datum b);

// Helpers (used by tests).
Datum MakeUuidDatum(const UuidData& uuid);
inline UuidData* DatumGetUuid(Datum x) {
    return reinterpret_cast<UuidData*>(x);
}

// Construct a UUID from raw 16 bytes (memcpy).
inline UuidData UuidFromBytes(const uint8_t b[16]) {
    UuidData u;
    std::memcpy(u.bytes, b, 16);
    return u;
}

}  // namespace pgcpp::types

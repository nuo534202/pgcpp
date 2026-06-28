#pragma once

#include <cstdint>
#include <limits>

#include "mytoydb/types/datum.hpp"

namespace mytoydb::types {

// ---------------------------------------------------------------------------
// Range types (PostgreSQL utils/adt/rangetypes.c).
//
// A generic range stores a (lower, upper) pair of element Datums with flags
// describing emptiness, inclusivity and bound presence. We expose a single
// generic RangeDatum struct that the various typed ranges
// (int4range/int8range/numrange/tsrange) build on.
//
// Storage: palloc'd RangeDatum; Datum is a pointer.
// ---------------------------------------------------------------------------

constexpr uint8_t kRangeEmpty = 0x01;
constexpr uint8_t kRangeLowerInclusive = 0x02;
constexpr uint8_t kRangeUpperInclusive = 0x04;
constexpr uint8_t kRangeLowerIsNull = 0x08;
constexpr uint8_t kRangeUpperIsNull = 0x10;  // unbounded above

struct RangeDatum {
    uint8_t flags = 0;
    Datum lower = 0;
    Datum upper = 0;
};

// Range constructors. typmod is the range's element-typmod; ignored here.
Datum int4range_in(const char* str);
char* int4range_out(Datum value);

Datum int8range_in(const char* str);
char* int8range_out(Datum value);

Datum numrange_in(const char* str);
char* numrange_out(Datum value);

Datum tsrange_in(const char* str);
char* tsrange_out(Datum value);

// Generic comparator: compares ranges by (lower-bound, upper-bound) and
// returns -1/0/1. Element comparator is selected by `element_oid`.
int range_cmp_internal(uint32_t element_oid, Datum a, Datum b);

// Range comparison operators (use int4range element semantics).
Datum range_eq(Datum a, Datum b);
Datum range_ne(Datum a, Datum b);
Datum range_lt(Datum a, Datum b);
Datum range_le(Datum a, Datum b);
Datum range_gt(Datum a, Datum b);
Datum range_ge(Datum a, Datum b);

// Containment: elem ? range -> bool (lower-case `?` is element-contained-in)
Datum range_contains_elem(Datum range, Datum elem);

// Helpers.
inline RangeDatum* DatumGetRange(Datum x) {
    return reinterpret_cast<RangeDatum*>(x);
}
Datum MakeRangeDatum(const RangeDatum& r);

}  // namespace mytoydb::types

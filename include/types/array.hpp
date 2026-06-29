#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "types/datum.hpp"

namespace pgcpp::types {

// ---------------------------------------------------------------------------
// Array type (PostgreSQL arrayfuncs.c).
//
// Storage: a palloc'd ArrayData. Datum is a pointer. The element type's OID
// is tracked so that elements can be returned as Datums of the right kind.
// NULL elements are recorded in a parallel `nulls` bitmap.
//
// This is a simplified in-memory representation: it does not implement the
// on-disk varlena array format. It is sufficient for in-executor array
// operations and for the SQL ARRAY[...] literal.
// ---------------------------------------------------------------------------

constexpr uint32_t kArrayHeaderSize = 4;

struct ArrayData {
    uint32_t element_oid;
    int32_t ndims;
    std::vector<int32_t> dims;
    std::vector<int32_t> lower_bounds;  // 1-based lower bounds per dim
    std::vector<Datum> values;          // flat row-major
    std::vector<bool> nulls;            // true if NULL at same index
};

// array_in — parse a PostgreSQL array literal of the form '{1,2,3}' or
// '{{1,2},{3,4}}'. `element_oid` selects the parser used for each element.
Datum array_in(const char* str, uint32_t element_oid, int32_t typmod);
char* array_out(Datum value);

int array_cmp(Datum a, Datum b);
Datum array_eq(Datum a, Datum b);
Datum array_ne(Datum a, Datum b);
Datum array_lt(Datum a, Datum b);

// array_length(arr, dim) — length of given dimension (1-based).
Datum array_length(Datum array, Datum dim);

// array_append(arr, elem) — append elem to a 1-D array.
Datum array_append(Datum array, Datum element);

// array_ndims(arr) — number of dimensions (1-D => 1).
Datum array_ndims(Datum array);

// Helper: construct a 1-D array from a vector of Datums.
Datum MakeArrayDatum(uint32_t element_oid, const std::vector<Datum>& values);
inline ArrayData* DatumGetArray(Datum x) {
    return reinterpret_cast<ArrayData*>(x);
}

}  // namespace pgcpp::types

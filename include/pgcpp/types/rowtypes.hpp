#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pgcpp/types/datum.hpp"

namespace pgcpp::types {

// ---------------------------------------------------------------------------
// Row / composite types (PostgreSQL utils/adt/rowtypes.c).
//
// A RowData holds parallel arrays of (element OID, element Datum, is_null).
// Construction is by `row_in` parsing a PostgreSQL text tuple literal
// `"(val1,val2,val3)"` or by `make_row` from explicit values.
//
// Storage: palloc'd RowData; Datum is a pointer.
// ---------------------------------------------------------------------------

struct RowData {
    std::vector<uint32_t> element_oids;
    std::vector<Datum> values;
    std::vector<bool> nulls;
};

// row_in — parse a PostgreSQL composite literal.
//   - Empty string elements are treated as NULL.
//   - Surrounding double quotes preserve embedded commas/parens.
//   - typoid is the row type's OID (used to lookup element types); for the
//     simplified in-memory form we keep element types as int4 unless
//     overridden.
Datum row_in(const char* str, uint32_t typoid, int32_t typmod);
char* row_out(Datum value);

int row_cmp(Datum a, Datum b);
Datum row_eq(Datum a, Datum b);

// Helpers.
Datum MakeRowDatum(const RowData& row);
inline RowData* DatumGetRow(Datum x) {
    return reinterpret_cast<RowData*>(x);
}

}  // namespace pgcpp::types

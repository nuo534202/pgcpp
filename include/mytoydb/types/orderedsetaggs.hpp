#pragma once

#include <cstdint>
#include <vector>

#include "mytoydb/types/datum.hpp"

namespace mytoydb::types {

// ---------------------------------------------------------------------------
// Ordered-set aggregates (PostgreSQL utils/adt/orderedsetaggs.c).
//
// Implements:
//   mode()      — most frequent value
//   percentile_disc(p) — discrete percentile (returns an input value)
//   percentile_cont(p) — continuous percentile (interpolates)
//   percentile_disc/array, percentile_cont/array (with multiple fractions)
//
// The simplified in-memory form takes the sorted input set plus a fraction
// in [0,1] and returns the result.
// ---------------------------------------------------------------------------

// mode — returns the most frequent value (ties broken by first encountered).
Datum ordered_set_mode(const std::vector<Datum>& values, int (*cmp)(Datum, Datum));

// percentile_disc — discrete percentile. `p` is a float8 datum in [0,1].
// Returns the value at the 1-based position ceil(p * N).
Datum ordered_set_percentile_disc(const std::vector<Datum>& sorted_values, Datum p,
                                  int (*cmp)(Datum, Datum));

// percentile_cont — continuous percentile. Interpolates linearly between the
// two adjacent values. `sorted_values` is assumed already sorted ascending.
Datum ordered_set_percentile_cont_int4(const std::vector<Datum>& sorted_values, Datum p);
Datum ordered_set_percentile_cont_float8(const std::vector<Datum>& sorted_values, Datum p);

// Multiple fractions variant — returns an array of results.
std::vector<Datum> ordered_set_percentile_disc_array(const std::vector<Datum>& sorted_values,
                                                     const std::vector<Datum>& fractions,
                                                     int (*cmp)(Datum, Datum));

}  // namespace mytoydb::types

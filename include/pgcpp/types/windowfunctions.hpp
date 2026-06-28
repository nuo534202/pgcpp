#pragma once

#include <cstdint>
#include <vector>

#include "pgcpp/types/datum.hpp"

namespace pgcpp::types {

// ---------------------------------------------------------------------------
// Window functions (PostgreSQL utils/adt/windowfuncs.c).
//
// The actual window framing is handled by the executor (nodeWindowAgg). The
// helpers below provide the per-frame state objects used by those window
// functions that need persistent state across rows.
// ---------------------------------------------------------------------------

// row_number — emits 1-based row numbers in the current partition. State is
// the running counter (int8).
Datum row_number_next(Datum state);
Datum row_number_current(Datum state);
Datum row_number_reset();

// rank — emits 1-based rank; ties get equal rank; next row gets +count.
struct RankState {
    int64_t row_count;
    int64_t rank;
    Datum last_partition_value;
};

Datum rank_init();
Datum rank_advance(Datum state, Datum current_value);
Datum rank_value(Datum state);

// dense_rank — emits 1-based dense rank; ties get equal rank; next row gets +1.
Datum dense_rank_init();
Datum dense_rank_advance(Datum state, Datum current_value);
Datum dense_rank_value(Datum state);

// lag / lead — return the element `offset` rows behind/ahead of the current
// row, defaulting to `default_value` when out of bounds.
Datum lag_compute(const std::vector<Datum>& partition, int64_t idx, int64_t offset,
                  Datum default_value);
Datum lead_compute(const std::vector<Datum>& partition, int64_t idx, int64_t offset,
                   Datum default_value);

// first_value / last_value / nth_value — return the requested element of the
// window frame.
Datum first_value(const std::vector<Datum>& frame);
Datum last_value(const std::vector<Datum>& frame);
Datum nth_value(const std::vector<Datum>& frame, int64_t n);

}  // namespace pgcpp::types

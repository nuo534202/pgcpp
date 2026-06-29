// windowfunctions.cpp — implementations of window function helpers.

#include "types/windowfunctions.hpp"

#include <cstdint>

#include "common/error/elog.hpp"
#include "common/memory/memory_context.hpp"

namespace pgcpp::types {

using pgcpp::error::LogLevel;
using pgcpp::memory::palloc;

Datum row_number_next(Datum state) {
    int64_t n = DatumGetInt64(state);
    return Int64GetDatum(n + 1);
}

Datum row_number_current(Datum state) {
    return state;
}

Datum row_number_reset() {
    return Int64GetDatum(0);
}

Datum rank_init() {
    auto* p = static_cast<RankState*>(palloc(sizeof(RankState)));
    p->row_count = 0;
    p->rank = 0;
    p->last_partition_value = 0;
    return reinterpret_cast<Datum>(p);
}

Datum rank_advance(Datum state, Datum current_value) {
    auto* s = reinterpret_cast<RankState*>(state);
    ++s->row_count;
    if (s->row_count == 1 || current_value != s->last_partition_value) {
        s->rank = s->row_count;
        s->last_partition_value = current_value;
    }
    return state;
}

Datum rank_value(Datum state) {
    const auto* s = reinterpret_cast<RankState*>(state);
    return Int64GetDatum(s->rank);
}

Datum dense_rank_init() {
    auto* p = static_cast<RankState*>(palloc(sizeof(RankState)));
    p->row_count = 0;
    p->rank = 0;
    p->last_partition_value = 0;
    return reinterpret_cast<Datum>(p);
}

Datum dense_rank_advance(Datum state, Datum current_value) {
    auto* s = reinterpret_cast<RankState*>(state);
    ++s->row_count;
    if (s->row_count == 1 || current_value != s->last_partition_value) {
        ++s->rank;
        s->last_partition_value = current_value;
    }
    return state;
}

Datum dense_rank_value(Datum state) {
    const auto* s = reinterpret_cast<RankState*>(state);
    return Int64GetDatum(s->rank);
}

Datum lag_compute(const std::vector<Datum>& partition, int64_t idx, int64_t offset,
                  Datum default_value) {
    int64_t target = idx - offset;
    if (target < 0 || target >= static_cast<int64_t>(partition.size())) {
        return default_value;
    }
    return partition[static_cast<std::size_t>(target)];
}

Datum lead_compute(const std::vector<Datum>& partition, int64_t idx, int64_t offset,
                   Datum default_value) {
    int64_t target = idx + offset;
    if (target < 0 || target >= static_cast<int64_t>(partition.size())) {
        return default_value;
    }
    return partition[static_cast<std::size_t>(target)];
}

Datum first_value(const std::vector<Datum>& frame) {
    if (frame.empty()) {
        return 0;
    }
    return frame.front();
}

Datum last_value(const std::vector<Datum>& frame) {
    if (frame.empty()) {
        return 0;
    }
    return frame.back();
}

Datum nth_value(const std::vector<Datum>& frame, int64_t n) {
    if (n < 1 || static_cast<std::size_t>(n) > frame.size()) {
        return 0;
    }
    return frame[static_cast<std::size_t>(n - 1)];
}

}  // namespace pgcpp::types

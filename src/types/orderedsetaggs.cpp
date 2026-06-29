// orderedsetaggs.cpp — implementations of ordered-set aggregate helpers.

#include "types/orderedsetaggs.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "common/error/elog.hpp"
#include "common/memory/memory_context.hpp"
#include "types/builtins.hpp"

namespace pgcpp::types {

using pgcpp::error::LogLevel;

Datum ordered_set_mode(const std::vector<Datum>& values, int (*cmp)(Datum, Datum)) {
    if (values.empty()) {
        return 0;
    }
    std::vector<Datum> sorted = values;
    std::sort(sorted.begin(), sorted.end(), [cmp](Datum a, Datum b) { return cmp(a, b) < 0; });
    Datum mode = sorted[0];
    int64_t mode_count = 1;
    int64_t current_count = 1;
    for (std::size_t i = 1; i < sorted.size(); ++i) {
        if (cmp(sorted[i], sorted[i - 1]) == 0) {
            ++current_count;
        } else {
            if (current_count > mode_count) {
                mode_count = current_count;
                mode = sorted[i - 1];
            }
            current_count = 1;
        }
    }
    if (current_count > mode_count) {
        mode = sorted.back();
    }
    return mode;
}

Datum ordered_set_percentile_disc(const std::vector<Datum>& sorted_values, Datum p,
                                  int (* /*cmp*/)(Datum, Datum)) {
    if (sorted_values.empty()) {
        return 0;
    }
    double fraction = DatumGetFloat8(p);
    if (fraction < 0 || fraction > 1) {
        ereport(LogLevel::kError, "percentile fraction must be in [0,1]");
    }
    int64_t n = static_cast<int64_t>(sorted_values.size());
    int64_t idx = static_cast<int64_t>(std::ceil(fraction * n));
    if (idx < 1) {
        idx = 1;
    }
    if (idx > n) {
        idx = n;
    }
    return sorted_values[static_cast<std::size_t>(idx - 1)];
}

Datum ordered_set_percentile_cont_int4(const std::vector<Datum>& sorted_values, Datum p) {
    if (sorted_values.empty()) {
        return Float8GetDatum(0.0);
    }
    double fraction = DatumGetFloat8(p);
    if (fraction < 0 || fraction > 1) {
        ereport(LogLevel::kError, "percentile fraction must be in [0,1]");
    }
    int64_t n = static_cast<int64_t>(sorted_values.size());
    double pos = fraction * (n - 1);
    int64_t lower = static_cast<int64_t>(std::floor(pos));
    int64_t upper = static_cast<int64_t>(std::ceil(pos));
    double frac = pos - lower;
    double low_val =
        static_cast<double>(DatumGetInt32(sorted_values[static_cast<std::size_t>(lower)]));
    double high_val =
        static_cast<double>(DatumGetInt32(sorted_values[static_cast<std::size_t>(upper)]));
    return Float8GetDatum(low_val + (high_val - low_val) * frac);
}

Datum ordered_set_percentile_cont_float8(const std::vector<Datum>& sorted_values, Datum p) {
    if (sorted_values.empty()) {
        return Float8GetDatum(0.0);
    }
    double fraction = DatumGetFloat8(p);
    if (fraction < 0 || fraction > 1) {
        ereport(LogLevel::kError, "percentile fraction must be in [0,1]");
    }
    int64_t n = static_cast<int64_t>(sorted_values.size());
    double pos = fraction * (n - 1);
    int64_t lower = static_cast<int64_t>(std::floor(pos));
    int64_t upper = static_cast<int64_t>(std::ceil(pos));
    double frac = pos - lower;
    double low_val = DatumGetFloat8(sorted_values[static_cast<std::size_t>(lower)]);
    double high_val = DatumGetFloat8(sorted_values[static_cast<std::size_t>(upper)]);
    return Float8GetDatum(low_val + (high_val - low_val) * frac);
}

std::vector<Datum> ordered_set_percentile_disc_array(const std::vector<Datum>& sorted_values,
                                                     const std::vector<Datum>& fractions,
                                                     int (*cmp)(Datum, Datum)) {
    std::vector<Datum> out;
    out.reserve(fractions.size());
    for (Datum f : fractions) {
        out.push_back(ordered_set_percentile_disc(sorted_values, f, cmp));
    }
    return out;
}

}  // namespace pgcpp::types

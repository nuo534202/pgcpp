// selfuncs.cpp — simplified selectivity estimation helpers.

#include "mytoydb/types/selfuncs.hpp"

#include <algorithm>
#include <cstdint>

namespace mytoydb::types {

double eqsel(double null_fraction, int32_t n_distinct, bool most_common_value_match) {
    if (n_distinct <= 0) {
        return 0.0;
    }
    double non_null = 1.0 - null_fraction;
    if (most_common_value_match) {
        // Selectivity of the MCV entry. Without further statistics we assume
        // uniform selectivity.
        return non_null / static_cast<double>(n_distinct);
    }
    return non_null * 0.5 / static_cast<double>(n_distinct);
}

double neqsel(double null_fraction, int32_t n_distinct, bool mcv_match) {
    double sel = 1.0 - eqsel(null_fraction, n_distinct, mcv_match);
    sel -= null_fraction;
    if (sel < 0) {
        sel = 0;
    }
    return sel;
}

double scalarltsel(double v) {
    if (v <= 0) {
        return 0.0;
    }
    if (v >= 1) {
        return 1.0;
    }
    return v;
}

double scalarlesel(double v) {
    if (v < 0) {
        return 0.0;
    }
    if (v >= 1) {
        return 1.0;
    }
    return v;
}

double scalargtsel(double v) {
    if (v <= 0) {
        return 1.0;
    }
    if (v >= 1) {
        return 0.0;
    }
    return 1.0 - v;
}

double scalargesel(double v) {
    if (v <= 0) {
        return 1.0;
    }
    if (v > 1) {
        return 0.0;
    }
    return 1.0 - v;
}

double eqjoinsel_inner(int32_t n_distinct_a, int32_t n_distinct_b) {
    int32_t n = (n_distinct_a > n_distinct_b) ? n_distinct_a : n_distinct_b;
    if (n <= 0) {
        return 0.0;
    }
    return 1.0 / static_cast<double>(n);
}

double cost_qual_eval_walker(int32_t num_clauses) {
    if (num_clauses <= 0) {
        return 0.0;
    }
    return static_cast<double>(num_clauses) * 0.0025;  // 2.5x CPU_PAGE_COST per op
}

}  // namespace mytoydb::types

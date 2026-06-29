#pragma once

#include <cstdint>

#include "types/datum.hpp"

namespace pgcpp::types {

// ---------------------------------------------------------------------------
// Selectivity estimation helpers (PostgreSQL utils/adt/selfuncs.c).
//
// These functions are the simplified equivalents of PostgreSQL's
// selectivity estimation framework. The real implementation reads pg_statistic
// histograms / MCV / correlation; we expose a small closed-form estimator so
// that the planner tests can be exercised.
// ---------------------------------------------------------------------------

// eqsel — selectivity of `=` against a constant.
double eqsel(double null_fraction, int32_t n_distinct, bool most_common_value_match);

// neqsel — selectivity of `<>`.
double neqsel(double null_fraction, int32_t n_distinct, bool mcv_match);

// scalarltsel / scalarle_sel — selectivity of `<` and `<=` over a value
// distribution with [0,1] normalised histogram and a target value `v`.
double scalarltsel(double v);
double scalarlesel(double v);

// scalargtsel / scalargesel — selectivity of `>` and `>=`.
double scalargtsel(double v);
double scalargesel(double v);

// Generic join selectivity — placeholder approximation: 1 / max(n_distinct_a,
// n_distinct_b).
double eqjoinsel_inner(int32_t n_distinct_a, int32_t n_distinct_b);

// Cost helpers — rough per-tuple cost contributions used by the planner
// tests. Real numbers come from costsize.c, these are the simplified
// invariants used by the unit tests.
double cost_qual_eval_walker(int32_t num_clauses);

}  // namespace pgcpp::types

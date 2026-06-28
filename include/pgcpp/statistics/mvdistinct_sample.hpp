// mvdistinct_sample.hpp — single-column ndistinct estimator + sample helper
// (M9 sub-task 15.20.5).
//
// Converts the estimator portion of PostgreSQL's src/backend/statistics/
// mvndistinct.c (estimate_ndistinct) and the sample-collection helper from
// src/backend/utils/adt/selfuncs.c / src/backend/commands/analyze.c to C++20.
//
// The Haas-Stefanski estimator (Haas & Stokes, "Estimating the Number of
// Classes in a Finite Population", 1998) extrapolates the number of distinct
// values in a population from a uniform random sample. PostgreSQL's
// mvndistinct.c uses the D1 estimator from that paper.

#pragma once

#include <vector>

#include "pgcpp/statistics/mvdistinct.hpp"
#include "pgcpp/types/datum.hpp"

namespace mytoydb::statistics {

// EstimateNDistinct — the Haas-Stefanski D1 estimator for a single column.
//   totalrows:  total rows in the relation (0 = sample is the whole relation)
//   samplerows: number of rows in the sample
//   distinct:   number of distinct values observed in the sample
//   f1:         number of values appearing exactly once in the sample
// Returns the estimated number of distinct values in the population.
double EstimateNDistinct(double totalrows, double samplerows, double distinct, double f1);

// SampleRows — build a StatsBuildData from flat value/null arrays.
// `values` and `nulls` are row-major with `attnums.size()` entries per row
// and `nrows` rows. The vectors are copied into the returned struct.
StatsBuildData SampleRows(const std::vector<AttrNumber>& attnums,
                          const std::vector<types::Datum>& values, const std::vector<bool>& nulls,
                          int nrows);

}  // namespace mytoydb::statistics

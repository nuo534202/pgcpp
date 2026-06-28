// mvdistinct.hpp — multi-column ndistinct estimates (M9 sub-task 15.20.5).
//
// Converts PostgreSQL's src/backend/statistics/mvndistinct.c (and the public
// structures from src/include/statistics/statistics.h) to C++20.
//
// A multi-column ndistinct coefficient is the estimated number of distinct
// values of a given combination of columns. PostgreSQL stores these as a
// serialized MVNDistinct blob in pg_statistic_ext_data.stxdndistinct.
//
// The Haas-Stefanski estimator (Haas & Stokes 1998) is used to extrapolate
// from a sample to the full population; see mvdistinct_sample.hpp for the
// single-column estimator and mvdistinct_sample.cpp for its implementation.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "pgcpp/types/datum.hpp"

namespace mytoydb::statistics {

// AttrNumber — PostgreSQL attribute number (1-based). Redeclared here so
// this header is self-contained; identical to the alias in extended_stats.hpp.
using AttrNumber = int;

// StatsBuildData — the input rows for building extended statistics.
// Mirrors PostgreSQL's StatsBuildData: a flat row-major values array plus
// matching nulls. All extended-statistic builders consume this struct.
struct StatsBuildData {
    std::vector<AttrNumber> attnums;   // attribute numbers (1-based)
    std::vector<types::Datum> values;  // row-major: values[row*nattrs+attr]
    std::vector<bool> nulls;           // row-major: nulls[row*nattrs+attr]
    int nrows = 0;                     // number of sample rows

    int NumAttrs() const { return static_cast<int>(attnums.size()); }
    types::Datum GetValue(int row, int attr) const {
        return values[static_cast<size_t>(row) * attnums.size() + attr];
    }
    bool IsNull(int row, int attr) const {
        return nulls[static_cast<size_t>(row) * attnums.size() + attr];
    }
};

// MVNDistinctItem — one (attributes -> ndistinct) estimate.
// `attrs` is a bitmask: bit i is set when attribute attnums[i] (i.e. column
// number i+1 in the build data) is part of the combination. This matches the
// bitmap encoding PostgreSQL uses (a Bitmapset serialized to a uint32 here,
// sufficient for the at-most-8-column case the simplified builder supports).
struct MVNDistinctItem {
    uint32_t attrs = 0;
    double ndistinct = 0.0;
};

// MVNDistinct — the full set of multi-column ndistinct estimates for one
// statistics object. One item per column combination of size >= 2.
struct MVNDistinct {
    std::vector<MVNDistinctItem> items;
};

// BuildMVNDistinct — compute ndistinct estimates for all column combinations
// of size >= 2 in `data`. `totalrows` is the full relation size (0 means the
// sample is the whole relation); it scales the sample estimate up.
MVNDistinct BuildMVNDistinct(const StatsBuildData& data, double totalrows);

// EstimateMVNDistinct — look up the ndistinct estimate for the attribute
// combination encoded by `attrs`. Returns -1.0 when no matching item exists.
double EstimateMVNDistinct(const MVNDistinct& nd, uint32_t attrs);

// SerializeMVNDistinct — produce a compact byte string suitable for storage
// in pg_statistic_ext_data.stxdndistinct (bytea).
std::string SerializeMVNDistinct(const MVNDistinct& nd);

// DeserializeMVNDistinct — inverse of SerializeMVNDistinct. Returns an empty
// MVNDistinct when the input is malformed or carries the wrong magic byte.
MVNDistinct DeserializeMVNDistinct(std::string_view data);

}  // namespace mytoydb::statistics

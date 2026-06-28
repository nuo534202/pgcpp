// mvdependencies.hpp — soft functional dependencies (M9 sub-task 15.20.5).
//
// Converts PostgreSQL's src/backend/statistics/mvdependencies.c (and the
// public structures from src/include/statistics/statistics.h) to C++20.
//
// A functional dependency A1...Ak -> B states that B is (approximately)
// determined by (A1, ..., Ak). The "degree" is the strength of the
// dependency in [0, 1]: 1.0 means B is fully determined, 0.0 means no
// dependency. The planner uses these to avoid overestimating the
// cardinality of joins and groupings on correlated columns.
//
// PostgreSQL stores a serialized MVDependencies blob in
// pg_statistic_ext_data.stxddependencies.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mytoydb::statistics {

// AttrNumber — PostgreSQL attribute number (1-based). Redeclared here so
// this header is self-contained; identical to the alias in extended_stats.hpp.
using AttrNumber = int;

// Forward declaration to avoid a circular include with mvdistinct.hpp.
struct StatsBuildData;

// MVDependencyAttr — one attribute participating in a dependency.
// `is_eq` marks an equality-match attribute (true for the equality operator
// used to build the dependency; PostgreSQL tracks this to support
// ORDER BY-based dependencies, but here it is always true).
struct MVDependencyAttr {
    AttrNumber attnum = 0;
    bool is_eq = true;
};

// MVDependency — a single soft functional dependency.
// attributes[0 .. k-1] are the determining (left-hand) attributes;
// attributes[k] is the dependent (right-hand) attribute. `degree` is the
// measured strength of the dependency over the sample.
struct MVDependency {
    std::vector<MVDependencyAttr> attributes;
    double degree = 0.0;
};

// MVDependencies — collection of functional dependencies for one stats object.
struct MVDependencies {
    std::vector<MVDependency> items;
};

// BuildMVDependencies — compute all single-determining-attribute functional
// dependencies (one per ordered (a, b) pair with a != b) from the sample.
MVDependencies BuildMVDependencies(const StatsBuildData& data);

// EstimateMVDependencies — return the strongest dependency whose determining
// attributes are a subset of `attrs` (bitmask, same encoding as
// MVNDistinctItem.attrs). Returns nullptr when no dependency applies.
const MVDependency* EstimateMVDependencies(const MVDependencies& deps, uint32_t attrs);

// SerializeMVDependencies — produce a byte string for storage in
// pg_statistic_ext_data.stxddependencies (bytea).
std::string SerializeMVDependencies(const MVDependencies& deps);

// DeserializeMVDependencies — inverse of SerializeMVDependencies.
MVDependencies DeserializeMVDependencies(std::string_view data);

}  // namespace mytoydb::statistics

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "catalog/catalog.hpp"

namespace pgcpp::catalog {

// FormData_pg_statistic — C++ equivalent of PostgreSQL's catalog/pg_statistic.h.
//
// Each row stores statistics about one column of one table, populated by
// ANALYZE and consumed by the planner/optimizer. Field names and semantics
// are preserved from PostgreSQL.

// stadistinct values: positive = exact count, negative = fraction of rows.
// stakind* values: STATISTIC_KIND_* constants (MCV, HISTOGRAM, etc.).

constexpr int kStatisticKindMcv = 1;          // most common values
constexpr int kStatisticKindHistogram = 2;    // histogram of values
constexpr int kStatisticKindCorrelation = 3;  // physical-vs-logical correlation
constexpr int kStatisticKindMcelem = 4;       // MCV for array elements
constexpr int kStatisticKindDechist = 5;      // distinct-element histogram

struct FormData_pg_statistic {
    Oid starelid = kInvalidOid;  // OID of the relation containing the column
    int16_t staattnum = 0;       // attnum of the column
    bool stainherit = false;     // is this an inherited stats row?
    float stanullfrac = 0.0F;    // fraction of entries that are NULL
    int32_t stawidth = 0;        // average stored width in bytes
    int32_t stadistinct = 0;     // distinct values: positive count, negative fraction
    // stakind1..5, staop1..5, stacoll1..5: kind/op/collation for each slot.
    int16_t stakind1 = 0;
    int16_t stakind2 = 0;
    int16_t stakind3 = 0;
    int16_t stakind4 = 0;
    int16_t stakind5 = 0;
    Oid staop1 = kInvalidOid;
    Oid staop2 = kInvalidOid;
    Oid staop3 = kInvalidOid;
    Oid staop4 = kInvalidOid;
    Oid staop5 = kInvalidOid;
    Oid stacoll1 = kInvalidOid;
    Oid stacoll2 = kInvalidOid;
    Oid stacoll3 = kInvalidOid;
    Oid stacoll4 = kInvalidOid;
    Oid stacoll5 = kInvalidOid;
    // PG stores values as anyarray; here we keep placeholders for serialization.
    std::string stavalues1;  // placeholder for serialized stats values
    std::string stavalues2;
    std::string stavalues3;
    std::string stavalues4;
    std::string stavalues5;
};

using Form_pg_statistic = FormData_pg_statistic*;

}  // namespace pgcpp::catalog

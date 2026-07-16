// stats_view.hpp — pg_stat_* virtual view definitions (P3-9).
//
// Defines the OID constants and column specifications for the pg_stat_*
// system views. These views are "virtual" — they have no physical heap
// file (relfilenode = kInvalidOid). When the executor scans one of these
// relations, it calls into StatisticsCollector to materialize the rows
// (see stats_scan.cpp).
//
// The OID range 6000-6099 is reserved for pg_stat_* views, chosen to avoid
// conflicts with PostgreSQL's built-in catalog OIDs (< 16384) and pgcpp's
// own assigned OIDs.
//
// Reference: PostgreSQL src/include/catalog/pg_statistic_ext.h and
//            src/backend/catalog/system_views.sql

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "catalog/catalog.hpp"

namespace pgcpp::stats {

// OID constants for pg_stat_* views.
constexpr pgcpp::catalog::Oid kPgStatDatabaseOid = 6000;
constexpr pgcpp::catalog::Oid kPgStatActivityOid = 6001;
constexpr pgcpp::catalog::Oid kPgStatAllTablesOid = 6002;
constexpr pgcpp::catalog::Oid kPgStatAllIndexesOid = 6003;

// Column specification for a stats view.
struct StatsColumn {
    std::string name;
    pgcpp::catalog::Oid type_oid;
    int16_t type_len;  // -1 for variable-length
    bool byval;
};

// Column definitions for each view.
const std::vector<StatsColumn>& GetPgStatDatabaseColumns();
const std::vector<StatsColumn>& GetPgStatActivityColumns();
const std::vector<StatsColumn>& GetPgStatAllTablesColumns();
const std::vector<StatsColumn>& GetPgStatAllIndexesColumns();

// Returns true if `relid` is a pg_stat_* view OID.
bool IsStatsView(pgcpp::catalog::Oid relid);

// Returns the column definitions for the given stats view OID.
// Returns nullptr if `relid` is not a stats view.
const std::vector<StatsColumn>* GetStatsViewColumns(pgcpp::catalog::Oid relid);

// Returns the view name (e.g. "pg_stat_database") for the given OID.
// Returns empty string if not a stats view.
std::string GetStatsViewName(pgcpp::catalog::Oid relid);

}  // namespace pgcpp::stats

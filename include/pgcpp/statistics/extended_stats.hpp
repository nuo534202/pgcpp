// extended_stats.hpp — extended statistics main API (M9 sub-task 15.20.5).
//
// Converts PostgreSQL's src/backend/statistics/extended_stats.c (and the
// public portions of src/include/statistics/statistics.h) to C++20.
//
// Extended statistics (CREATE STATISTICS ...) are multi-column statistics
// objects stored in pg_statistic_ext. Each object records which columns and
// which statistic kinds (ndistinct / dependencies / mcv) apply. The actual
// per-kind data lives in pg_statistic_ext_data and is built by the
// mvdistinct / mvdependencies modules.
//
// This header defines:
//   - StatsExtKind enum (matching STATS_EXT_* macros)
//   - StatsExtData (a row in pg_statistic_ext)
//   - CreateStatisticsStmt / AlterStatisticsStmt / DropStatisticsStmt
//     (parser-node-shaped structs)
//   - StatisticsCatalog: an in-memory std::map cache keyed by stxoid
//   - StatisticsProvider: the per-kind handler registry
//   - SQL-facing CreateStatistics / AlterStatistics / RemoveStatistics

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace pgcpp::statistics {

// Oid — PostgreSQL object identifier. Local alias to avoid a hard dependency
// on catalog.hpp; identical to catalog::Oid (uint32_t).
using Oid = uint32_t;

// AttrNumber — PostgreSQL attribute number (1-based).
using AttrNumber = int;

// StatsExtKind — kind of extended statistic, matching PostgreSQL's
// STATS_EXT_NDISTINCT / STATS_EXT_DEPENDENCIES / STATS_EXT_MCV char codes
// stored in pg_statistic_ext.stxkind.
enum class StatsExtKind : char {
    kNdistinct = 'd',     // multi-column ndistinct coefficients
    kDependencies = 'f',  // soft functional dependencies
    kMcv = 'm',           // most-common values
};

// StatsExtData — a row of pg_statistic_ext. Describes one extended
// statistics object: which relation/columns it covers and which kinds are
// enabled. The built statistic values themselves (ndistinct/dependencies/mcv
// byte strings) are stored separately and looked up via the catalog OID.
struct StatsExtData {
    Oid stxoid = 0;                     // OID of the pg_statistic_ext row
    std::string stxname;                // statistics object name
    Oid stxnamespace = 0;               // namespace OID
    Oid stxrelid = 0;                   // relation the stats apply to
    int stxstattarget = -1;             // -1 = use default_statistics_target
    std::vector<StatsExtKind> stxkind;  // enabled statistic kinds
    std::vector<AttrNumber> stxkeys;    // attnums of covered columns
};

// CreateStatisticsStmt — CREATE STATISTICS statement (matches the shape of
// PostgreSQL's CreateStatsStmt parser node, renamed per project convention).
struct CreateStatisticsStmt {
    std::vector<std::string> stxnames;  // qualified name parts (last = name)
    std::vector<std::string> stxattrs;  // column names to cover
    std::vector<StatsExtKind> kinds;    // statistic kinds to build
    Oid stxrelid = 0;                   // relation OID (set by analysis)
    bool if_not_exists = false;
};

// AlterStatisticsStmt — ALTER STATISTICS ... SET STATISTICS N.
struct AlterStatisticsStmt {
    std::vector<std::string> stxnames;  // qualified name parts
    int stxstattarget = -1;             // new statistics target
    bool missing_ok = false;            // if true, no error when absent
};

// DropStatisticsStmt — DROP STATISTICS statement (PostgreSQL uses DropStmt
// for this; the struct here keeps a dedicated shape for clarity).
struct DropStatisticsStmt {
    std::vector<std::vector<std::string>> stxnames;  // list of qualified names
    bool if_exists = false;
    bool missing_ok = false;
};

// StatisticsCatalog — in-memory cache of extended statistics objects,
// keyed by stxoid. Mirrors the role of pg_statistic_ext without yet
// requiring the full catalog/syscache infrastructure. It is a singleton
// (function-local static) so that it survives across statements within a
// session and can be reset between tests.
class StatisticsCatalog {
public:
    static StatisticsCatalog& Instance();

    // SQL-facing operations.
    // CreateStatistics: validate the statement, allocate an OID, register
    // the object. Returns the new OID. With if_not_exists and a duplicate
    // name, returns the existing OID instead of erroring.
    Oid CreateStatistics(const CreateStatisticsStmt& stmt);
    // AlterStatistics: update stxstattarget by name.
    void AlterStatistics(const AlterStatisticsStmt& stmt);
    // RemoveStatistics: drop by OID.
    void RemoveStatistics(Oid stxoid);

    // Lookups.
    const StatsExtData* Lookup(Oid stxoid) const;
    const StatsExtData* LookupByName(const std::string& name) const;

    // Test helpers.
    void Clear();
    void SetNextOid(Oid next);
    Oid GetNextOid() const;

private:
    StatisticsCatalog() = default;
    std::map<Oid, StatsExtData> cache_;
    Oid next_oid_ = 10000;  // first user OID (system OIDs are < 10000)
};

// StatisticsProvider — registry of per-kind handlers. Each kind of extended
// statistic (ndistinct, dependencies, mcv) is built and applied by a
// provider. This mirrors PostgreSQL's stats provider pattern (see
// extended_stats.c's stats_handlers[]). Providers are registered as raw
// pointers; ownership remains with the caller (typically a static or a
// stack object that Unregisters before destruction).
class StatisticsProvider {
public:
    virtual ~StatisticsProvider() = default;
    virtual StatsExtKind Kind() const = 0;
    virtual const char* Name() const = 0;

    static void Register(StatisticsProvider* provider);
    static void Unregister(StatisticsProvider* provider);
    // Lookup by kind. Returns nullptr when no provider is registered.
    static StatisticsProvider* Lookup(StatsExtKind kind);
    // Clear all registrations (test helper).
    static void ClearRegistry();
};

// SQL-facing convenience wrappers around StatisticsCatalog::Instance().
Oid CreateStatistics(const CreateStatisticsStmt& stmt);
void AlterStatistics(const AlterStatisticsStmt& stmt);
void RemoveStatistics(Oid stxoid);

}  // namespace pgcpp::statistics

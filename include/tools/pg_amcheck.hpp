// pg_amcheck.h — Relation consistency check (pg_amcheck).
//
// Converted from PostgreSQL 15's src/bin/pg_amcheck/.
//
// pg_amcheck connects to a running server and invokes the amcheck extension
// functions (bt_index_check, bt_index_parent_check, verify_heapam) against
// the relations of one or more databases. It is intended for detecting
// on-disk corruption of heap and btree structures without taking the
// database down.
//
// PG's pg_amcheck supports parallel workers, per-table filtering, and a
// progress display; pgcpp's port is a single-threaded, sequential checker
// that issues the amcheck SQL via libpq and aggregates the results.
//
// The output is a textual summary (n_checked / n_corrupt / n_error) plus,
// when --verbose is set, one line per relation checked. The process exits
// 0 when no corruption/errors are found and non-zero otherwise.
#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace pgcpp::tools {

// AmcheckScope — what to check on a given relation.
enum class AmcheckScope {
    kHeap,    // verify_heapam(relation)
    kIndex,   // bt_index_check(index)
    kParent,  // bt_index_parent_check(index) — heavier, takes locks
};

// AmcheckOptions — inputs to a check run.
struct AmcheckOptions {
    std::string host = "localhost";
    int port = 5432;
    std::string dbname;         // database to connect to
    bool all_db = false;        // check all databases (template1 excluded)
    bool heapall = true;        // check heap relations
    bool index_check = true;    // run bt_index_check
    bool parent_check = false;  // run bt_index_parent_check (heavier)
    bool verbose = false;
    std::string table_pattern;  // substring filter on table name
    std::string index_pattern;  // substring filter on index name
};

// AmcheckResult — outcome of a check run.
enum class AmcheckResult {
    kOk,
    kConnectFailed,
    kCatalogQueryFailed,
    kCorruptionFound,
    kNoRelationsFound,
    kAmcheckExtensionMissing,
};

// AmcheckStats — counters accumulated during a run.
struct AmcheckStats {
    int databases_checked = 0;
    int relations_checked = 0;
    int indexes_checked = 0;
    int corrupt = 0;
    int errors = 0;

    bool AllOk() const { return corrupt == 0 && errors == 0; }
};

// RunAmcheck — connect to the server and check relations.
AmcheckResult RunAmcheck(const AmcheckOptions& opts, AmcheckStats& stats,
                         std::ostream* verbose_out = nullptr);

// --- Helpers (exposed for testing) ---

// BuildHeapCheckSql — emit a SELECT verify_heapam('<table>'::regclass) query.
std::string BuildHeapCheckSql(const std::string& table_name);

// BuildIndexCheckSql — emit a SELECT bt_index_check('<index>'::regclass).
std::string BuildIndexCheckSql(const std::string& index_name);

// BuildIndexParentCheckSql — emit bt_index_parent_check (heavier).
std::string BuildIndexParentCheckSql(const std::string& index_name);

// BuildListTablesSql — emit a query returning (relname) of user tables.
// NOTE: distinct from psql_describe's BuildListTablesSql (which returns a
// multi-column \dt result); this version returns a single relname column
// suitable for amcheck iteration.
std::string BuildAmcheckTableListSql(const std::string& table_pattern);

// BuildListIndexesSql — emit a query returning (indexrelname, relname).
std::string BuildListIndexesSql(const std::string& index_pattern);

// BuildListDatabasesSql — emit a query returning (datname) of databases
// that are not template databases. NOTE: distinct from psql_describe's
// BuildListDatabasesSql (which returns the multi-column \l result).
std::string BuildAmcheckDatabaseListSql();

// BuildCreateExtensionSql — emit "CREATE EXTENSION IF NOT EXISTS amcheck".
std::string BuildCreateExtensionSql();

// InterpretCheckResult — convert a query result value to a corruption flag.
// "true" or "t" => corruption detected; anything else => ok.
bool InterpretCheckResult(const std::string& value);

}  // namespace pgcpp::tools

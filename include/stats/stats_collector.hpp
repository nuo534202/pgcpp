// stats_collector.hpp — Runtime statistics collector (P3-9).
//
// In PostgreSQL, the statistics collector is a separate daemon process that
// receives UDP messages from backends and aggregates per-database, per-table,
// per-index, and per-backend runtime statistics. These are exposed via the
// pg_stat_* family of system views.
//
// pgcpp uses a single-process model, so the collector is an in-process
// singleton (StatisticsCollector) that backends report events to directly.
// The accumulated stats are queried by the pg_stat_* virtual relations
// (see stats_view.hpp and stats_scan.cpp).
//
// Reference: PostgreSQL src/backend/postmaster/pgstat.c

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace pgcpp::stats {

// --- Per-database statistics (pg_stat_database) ---
struct DatabaseStats {
    uint32_t datid = 0;           // database OID
    std::string datname;          // database name
    int64_t num_commit = 0;       // transactions committed
    int64_t num_rollback = 0;     // transactions rolled back
    int64_t blks_read = 0;        // disk blocks read
    int64_t blks_hit = 0;         // buffer hits
    int64_t tuples_returned = 0;  // tuples returned by queries
    int64_t tuples_fetched = 0;   // tuples fetched by index scans
    int64_t tuples_inserted = 0;
    int64_t tuples_updated = 0;
    int64_t tuples_deleted = 0;
    int64_t conflicts = 0;  // recovery conflicts (placeholder)
    int64_t temp_files = 0;
    int64_t temp_bytes = 0;
    int64_t deadlocks = 0;
    double session_time = 0.0;  // total session time (seconds)
    double active_time = 0.0;   // time executing queries (seconds)
    int64_t sessions = 0;
    int64_t sessions_abandoned = 0;
    int64_t sessions_fatal = 0;
    int64_t sessions_killed = 0;
};

// --- Per-table statistics (pg_stat_all_tables) ---
struct TableStats {
    uint32_t relid = 0;            // table OID
    std::string relname;           // table name
    uint32_t schemoid = 0;         // schema OID
    std::string schemaname;        // schema name
    int64_t seq_scan = 0;          // sequential scans performed
    int64_t seq_tuples_read = 0;   // tuples read by seq scans
    int64_t idx_scan = 0;          // index scans (aggregated across all indexes)
    int64_t idx_tuples_fetch = 0;  // live tuples fetched by index scans
    int64_t n_tuples_ins = 0;
    int64_t n_tuples_upd = 0;
    int64_t n_tuples_del = 0;
    int64_t n_tuples_hot_upd = 0;
    int64_t n_live_tuples = 0;
    int64_t n_dead_tuples = 0;
    int64_t last_vacuum = 0;  // epoch seconds (0 = never)
    int64_t last_autovacuum = 0;
    int64_t last_analyze = 0;
    int64_t last_autoanalyze = 0;
};

// --- Per-index statistics (pg_stat_all_indexes) ---
struct IndexStats {
    uint32_t relid = 0;            // index OID
    std::string relname;           // index name
    uint32_t tableoid = 0;         // parent table OID
    std::string schemaname;        // schema name
    int64_t idx_scan = 0;          // index scans initiated
    int64_t idx_tuples_read = 0;   // index entries returned
    int64_t idx_tuples_fetch = 0;  // table rows fetched
};

// --- Per-backend activity (pg_stat_activity) ---
struct BackendInfo {
    int32_t pid = 0;      // backend PID
    uint32_t datid = 0;   // database OID
    std::string datname;  // database name
    uint32_t userid = 0;  // user OID
    std::string application_name;
    std::string state;          // "active", "idle", "idle in transaction"
    std::string query;          // current query text
    int64_t backend_start = 0;  // epoch seconds
    int64_t query_start = 0;
    int64_t xact_start = 0;
};

// StatisticsCollector — singleton aggregator for runtime stats.
//
// All methods are safe to call from the main backend thread (pgcpp is
// single-process). Stats are accumulated for the lifetime of the process
// (no periodic reset in the current implementation).
class StatisticsCollector {
public:
    StatisticsCollector() = default;
    ~StatisticsCollector() = default;
    StatisticsCollector(const StatisticsCollector&) = delete;
    StatisticsCollector& operator=(const StatisticsCollector&) = delete;

    // --- Reporting API (called by executor / transaction hooks) ---

    void ReportCommit(uint32_t dboid);
    void ReportAbort(uint32_t dboid);
    void ReportBlockRead(uint32_t dboid);
    void ReportBlockHit(uint32_t dboid);
    void ReportSeqScan(uint32_t relid, int64_t tuples_read);
    void ReportIndexScan(uint32_t indexoid, int64_t tuples_read, int64_t tuples_fetched);
    void ReportInsert(uint32_t relid, int64_t count);
    void ReportUpdate(uint32_t relid, int64_t count, int64_t hot_count = 0);
    void ReportDelete(uint32_t relid, int64_t count);
    void ReportTuplesReturned(uint32_t dboid, int64_t count);
    void ReportTuplesFetched(uint32_t dboid, int64_t count);

    // --- Database OID registration ---
    // Register a database so it appears in pg_stat_database with zero stats.
    void RegisterDatabase(uint32_t dboid, const std::string& name);
    // Set the "current" database OID for convenience reporting methods.
    void SetCurrentDatabase(uint32_t dboid);

    // --- Query API (called by stats view scan) ---

    // Returns a pointer to the stats for the given database, or nullptr.
    const DatabaseStats* GetDatabaseStats(uint32_t dboid) const;
    // Returns a snapshot of all database stats (copies for stability).
    std::vector<DatabaseStats> GetAllDatabaseStats() const;
    // Returns a snapshot of all table stats.
    std::vector<TableStats> GetAllTableStats() const;
    // Returns a snapshot of all index stats.
    std::vector<IndexStats> GetAllIndexStats() const;
    // Returns info about the current (single) backend.
    const BackendInfo& GetBackendInfo() const { return backend_; }
    // Update the backend activity info (called at query start/end).
    void UpdateBackendActivity(const std::string& query, const std::string& state);

    // --- Table/Index registration ---
    // Register a table so it appears in pg_stat_all_tables. Called by
    // BootstrapStatsViews and when tables are created.
    void RegisterTable(uint32_t relid, const std::string& relname, uint32_t schemoid,
                       const std::string& schemaname);
    void RegisterIndex(uint32_t indexoid, const std::string& relname, uint32_t tableoid,
                       const std::string& schemaname);

    // Direct access for testing (non-const).
    DatabaseStats* GetMutableDatabaseStats(uint32_t dboid);
    TableStats* GetMutableTableStats(uint32_t relid);

private:
    // datid -> stats
    std::map<uint32_t, DatabaseStats> db_stats_;
    // relid -> stats
    std::map<uint32_t, TableStats> table_stats_;
    // indexoid -> stats
    std::map<uint32_t, IndexStats> index_stats_;
    BackendInfo backend_;
    uint32_t current_db_ = 0;
};

// Global accessor. Returns the process-wide StatisticsCollector singleton.
// Lazily initialized on first call (so tests that don't need stats do not
// pay the cost).
StatisticsCollector& GetStatsCollector();

// Convenience wrapper: report a commit for the current database.
void ReportCommitCurrentDb();
void ReportAbortCurrentDb();

}  // namespace pgcpp::stats

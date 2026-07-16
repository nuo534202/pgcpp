// stats_view.cpp — pg_stat_* view column definitions and helpers (P3-9).
#include "stats/stats_view.hpp"

#include "types/datum.hpp"

namespace pgcpp::stats {

using pgcpp::catalog::Oid;
using pgcpp::types::kFloat8Oid;
using pgcpp::types::kInt4Oid;
using pgcpp::types::kInt8Oid;
using pgcpp::types::kTextOid;
using pgcpp::types::kTimestampOid;

// pg_stat_database columns:
//   datid oid, datname name, num_commit bigint, num_rollback bigint,
//   blks_read bigint, blks_hit bigint, tuples_returned bigint,
//   tuples_fetched bigint, tuples_inserted bigint, tuples_updated bigint,
//   tuples_deleted bigint, conflicts bigint, temp_files bigint,
//   temp_bytes bigint, deadlocks bigint
const std::vector<StatsColumn>& GetPgStatDatabaseColumns() {
    static const std::vector<StatsColumn> cols = {
        {"datid", kInt4Oid, 4, true},           {"datname", kTextOid, -1, false},
        {"num_commit", kInt8Oid, 8, true},      {"num_rollback", kInt8Oid, 8, true},
        {"blks_read", kInt8Oid, 8, true},       {"blks_hit", kInt8Oid, 8, true},
        {"tuples_returned", kInt8Oid, 8, true}, {"tuples_fetched", kInt8Oid, 8, true},
        {"tuples_inserted", kInt8Oid, 8, true}, {"tuples_updated", kInt8Oid, 8, true},
        {"tuples_deleted", kInt8Oid, 8, true},  {"conflicts", kInt8Oid, 8, true},
        {"temp_files", kInt8Oid, 8, true},      {"temp_bytes", kInt8Oid, 8, true},
        {"deadlocks", kInt8Oid, 8, true},
    };
    return cols;
}

// pg_stat_activity columns:
//   pid integer, datid oid, datname name, userid oid,
//   application_name text, state text, query text,
//   backend_start timestamp, query_start timestamp, xact_start timestamp
const std::vector<StatsColumn>& GetPgStatActivityColumns() {
    static const std::vector<StatsColumn> cols = {
        {"pid", kInt4Oid, 4, true},
        {"datid", kInt4Oid, 4, true},
        {"datname", kTextOid, -1, false},
        {"userid", kInt4Oid, 4, true},
        {"application_name", kTextOid, -1, false},
        {"state", kTextOid, -1, false},
        {"query", kTextOid, -1, false},
        {"backend_start", kTimestampOid, 8, true},
        {"query_start", kTimestampOid, 8, true},
        {"xact_start", kTimestampOid, 8, true},
    };
    return cols;
}

// pg_stat_all_tables columns:
//   relid oid, schemaname name, relname name,
//   seq_scan bigint, seq_tuples_read bigint,
//   idx_scan bigint, idx_tuples_fetch bigint,
//   n_tuples_ins bigint, n_tuples_upd bigint, n_tuples_del bigint,
//   n_tuples_hot_upd bigint, n_live_tuples bigint, n_dead_tuples bigint,
//   last_vacuum timestamp, last_autovacuum timestamp,
//   last_analyze timestamp, last_autoanalyze timestamp
const std::vector<StatsColumn>& GetPgStatAllTablesColumns() {
    static const std::vector<StatsColumn> cols = {
        {"relid", kInt4Oid, 4, true},
        {"schemaname", kTextOid, -1, false},
        {"relname", kTextOid, -1, false},
        {"seq_scan", kInt8Oid, 8, true},
        {"seq_tuples_read", kInt8Oid, 8, true},
        {"idx_scan", kInt8Oid, 8, true},
        {"idx_tuples_fetch", kInt8Oid, 8, true},
        {"n_tuples_ins", kInt8Oid, 8, true},
        {"n_tuples_upd", kInt8Oid, 8, true},
        {"n_tuples_del", kInt8Oid, 8, true},
        {"n_tuples_hot_upd", kInt8Oid, 8, true},
        {"n_live_tuples", kInt8Oid, 8, true},
        {"n_dead_tuples", kInt8Oid, 8, true},
        {"last_vacuum", kTimestampOid, 8, true},
        {"last_autovacuum", kTimestampOid, 8, true},
        {"last_analyze", kTimestampOid, 8, true},
        {"last_autoanalyze", kTimestampOid, 8, true},
    };
    return cols;
}

// pg_stat_all_indexes columns:
//   relid oid, schemaname name, relname name,
//   tableoid oid, idx_scan bigint,
//   idx_tuples_read bigint, idx_tuples_fetch bigint
const std::vector<StatsColumn>& GetPgStatAllIndexesColumns() {
    static const std::vector<StatsColumn> cols = {
        {"relid", kInt4Oid, 4, true},
        {"schemaname", kTextOid, -1, false},
        {"relname", kTextOid, -1, false},
        {"tableoid", kInt4Oid, 4, true},
        {"idx_scan", kInt8Oid, 8, true},
        {"idx_tuples_read", kInt8Oid, 8, true},
        {"idx_tuples_fetch", kInt8Oid, 8, true},
    };
    return cols;
}

bool IsStatsView(Oid relid) {
    return relid == kPgStatDatabaseOid || relid == kPgStatActivityOid ||
           relid == kPgStatAllTablesOid || relid == kPgStatAllIndexesOid;
}

const std::vector<StatsColumn>* GetStatsViewColumns(Oid relid) {
    switch (relid) {
        case kPgStatDatabaseOid:
            return &GetPgStatDatabaseColumns();
        case kPgStatActivityOid:
            return &GetPgStatActivityColumns();
        case kPgStatAllTablesOid:
            return &GetPgStatAllTablesColumns();
        case kPgStatAllIndexesOid:
            return &GetPgStatAllIndexesColumns();
        default:
            return nullptr;
    }
}

std::string GetStatsViewName(Oid relid) {
    switch (relid) {
        case kPgStatDatabaseOid:
            return "pg_stat_database";
        case kPgStatActivityOid:
            return "pg_stat_activity";
        case kPgStatAllTablesOid:
            return "pg_stat_all_tables";
        case kPgStatAllIndexesOid:
            return "pg_stat_all_indexes";
        default:
            return {};
    }
}

}  // namespace pgcpp::stats

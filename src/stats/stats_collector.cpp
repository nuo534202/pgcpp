// stats_collector.cpp — StatisticsCollector implementation (P3-9).
#include "stats/stats_collector.hpp"

namespace pgcpp::stats {

void StatisticsCollector::ReportCommit(uint32_t dboid) {
    auto& s = db_stats_[dboid];
    s.datid = dboid;
    ++s.num_commit;
}

void StatisticsCollector::ReportAbort(uint32_t dboid) {
    auto& s = db_stats_[dboid];
    s.datid = dboid;
    ++s.num_rollback;
}

void StatisticsCollector::ReportBlockRead(uint32_t dboid) {
    auto& s = db_stats_[dboid];
    s.datid = dboid;
    ++s.blks_read;
}

void StatisticsCollector::ReportBlockHit(uint32_t dboid) {
    auto& s = db_stats_[dboid];
    s.datid = dboid;
    ++s.blks_hit;
}

void StatisticsCollector::ReportSeqScan(uint32_t relid, int64_t tuples_read) {
    auto& s = table_stats_[relid];
    s.relid = relid;
    ++s.seq_scan;
    s.seq_tuples_read += tuples_read;
}

void StatisticsCollector::ReportIndexScan(uint32_t indexoid, int64_t tuples_read,
                                          int64_t tuples_fetched) {
    auto& s = index_stats_[indexoid];
    s.relid = indexoid;
    ++s.idx_scan;
    s.idx_tuples_read += tuples_read;
    s.idx_tuples_fetch += tuples_fetched;
}

void StatisticsCollector::ReportInsert(uint32_t relid, int64_t count) {
    auto& s = table_stats_[relid];
    s.relid = relid;
    s.n_tuples_ins += count;
    s.n_live_tuples += count;
}

void StatisticsCollector::ReportUpdate(uint32_t relid, int64_t count, int64_t hot_count) {
    auto& s = table_stats_[relid];
    s.relid = relid;
    s.n_tuples_upd += count;
    s.n_tuples_hot_upd += hot_count;
    // Net live tuples unchanged (one updated, one new).
}

void StatisticsCollector::ReportDelete(uint32_t relid, int64_t count) {
    auto& s = table_stats_[relid];
    s.relid = relid;
    s.n_tuples_del += count;
    s.n_live_tuples -= count;
    s.n_dead_tuples += count;
}

void StatisticsCollector::ReportTuplesReturned(uint32_t dboid, int64_t count) {
    auto& s = db_stats_[dboid];
    s.datid = dboid;
    s.tuples_returned += count;
}

void StatisticsCollector::ReportTuplesFetched(uint32_t dboid, int64_t count) {
    auto& s = db_stats_[dboid];
    s.datid = dboid;
    s.tuples_fetched += count;
}

void StatisticsCollector::RegisterDatabase(uint32_t dboid, const std::string& name) {
    auto& s = db_stats_[dboid];
    s.datid = dboid;
    if (s.datname.empty()) {
        s.datname = name;
    }
}

void StatisticsCollector::SetCurrentDatabase(uint32_t dboid) {
    current_db_ = dboid;
    backend_.datid = dboid;
    // Look up the database name if registered.
    auto it = db_stats_.find(dboid);
    if (it != db_stats_.end()) {
        backend_.datname = it->second.datname;
    }
}

const DatabaseStats* StatisticsCollector::GetDatabaseStats(uint32_t dboid) const {
    auto it = db_stats_.find(dboid);
    return (it != db_stats_.end()) ? &it->second : nullptr;
}

std::vector<DatabaseStats> StatisticsCollector::GetAllDatabaseStats() const {
    std::vector<DatabaseStats> out;
    out.reserve(db_stats_.size());
    for (const auto& [oid, stats] : db_stats_) {
        out.push_back(stats);
    }
    return out;
}

std::vector<TableStats> StatisticsCollector::GetAllTableStats() const {
    std::vector<TableStats> out;
    out.reserve(table_stats_.size());
    for (const auto& [oid, stats] : table_stats_) {
        out.push_back(stats);
    }
    return out;
}

std::vector<IndexStats> StatisticsCollector::GetAllIndexStats() const {
    std::vector<IndexStats> out;
    out.reserve(index_stats_.size());
    for (const auto& [oid, stats] : index_stats_) {
        out.push_back(stats);
    }
    return out;
}

void StatisticsCollector::UpdateBackendActivity(const std::string& query,
                                                const std::string& state) {
    backend_.query = query;
    backend_.state = state;
}

void StatisticsCollector::RegisterTable(uint32_t relid, const std::string& relname,
                                        uint32_t schemoid, const std::string& schemaname) {
    auto& s = table_stats_[relid];
    s.relid = relid;
    if (s.relname.empty()) {
        s.relname = relname;
    }
    s.schemoid = schemoid;
    s.schemaname = schemaname;
}

void StatisticsCollector::RegisterIndex(uint32_t indexoid, const std::string& relname,
                                        uint32_t tableoid, const std::string& schemaname) {
    auto& s = index_stats_[indexoid];
    s.relid = indexoid;
    if (s.relname.empty()) {
        s.relname = relname;
    }
    s.tableoid = tableoid;
    s.schemaname = schemaname;
}

DatabaseStats* StatisticsCollector::GetMutableDatabaseStats(uint32_t dboid) {
    auto it = db_stats_.find(dboid);
    return (it != db_stats_.end()) ? &it->second : nullptr;
}

TableStats* StatisticsCollector::GetMutableTableStats(uint32_t relid) {
    auto it = table_stats_.find(relid);
    return (it != table_stats_.end()) ? &it->second : nullptr;
}

// --- Global singleton ---

StatisticsCollector& GetStatsCollector() {
    static StatisticsCollector instance;
    return instance;
}

void ReportCommitCurrentDb() {
    GetStatsCollector().ReportCommit(GetStatsCollector().GetBackendInfo().datid);
}

void ReportAbortCurrentDb() {
    GetStatsCollector().ReportAbort(GetStatsCollector().GetBackendInfo().datid);
}

}  // namespace pgcpp::stats

// stats_scan.cpp — Virtual scan for pg_stat_* views (P3-9).
//
// Materializes rows from the StatisticsCollector into HeapTuples that the
// SeqScan executor can return. Each pg_stat_* view has a fixed tuple
// descriptor derived from its column definitions (stats_view.hpp).
#include "stats/stats_scan.hpp"

#include <cstring>
#include <memory>
#include <new>
#include <vector>

#include "access/heapam.hpp"
#include "access/rel.hpp"
#include "common/containers/node.hpp"
#include "common/memory/memory_context.hpp"
#include "stats/stats_collector.hpp"
#include "stats/stats_view.hpp"
#include "transaction/heap_tuple.hpp"
#include "types/datum.hpp"

namespace pgcpp::stats {

using pgcpp::access::CreateTupleDesc;
using pgcpp::access::TupleDesc;
using pgcpp::catalog::FormData_pg_attribute;
using pgcpp::catalog::Oid;
using pgcpp::memory::palloc;
using pgcpp::transaction::HeapTuple;
using pgcpp::transaction::HeapTupleData;
using pgcpp::types::Datum;
using pgcpp::types::Int32GetDatum;
using pgcpp::types::Int64GetDatum;
using pgcpp::types::TextPGetDatum;

namespace {

// Allocate a palloc'd varlena-style text Datum from a std::string.
// 4-byte total-length header + raw bytes (matches builtins.cpp::text_in).
Datum MakeTextDatum(const std::string& s) {
    std::size_t total = sizeof(int32_t) + s.size();
    char* buf = static_cast<char*>(palloc(total));
    int32_t header = static_cast<int32_t>(total);
    std::memcpy(buf, &header, sizeof(header));
    if (!s.empty()) {
        std::memcpy(buf + sizeof(int32_t), s.data(), s.size());
    }
    return TextPGetDatum(buf);
}

// Build a TupleDesc from the column specifications of a stats view.
TupleDesc BuildStatsTupleDesc(Oid view_oid) {
    const std::vector<StatsColumn>* cols = GetStatsViewColumns(view_oid);
    if (cols == nullptr) {
        return nullptr;
    }
    std::vector<FormData_pg_attribute> attrs;
    attrs.reserve(cols->size());
    for (std::size_t i = 0; i < cols->size(); ++i) {
        const StatsColumn& c = (*cols)[i];
        FormData_pg_attribute a;
        a.attrelid = view_oid;
        a.attname = c.name;
        a.atttypid = c.type_oid;
        a.attlen = c.type_len;
        a.attnum = static_cast<int16_t>(i + 1);
        a.attbyval = c.byval;
        a.attcacheoff = -1;
        a.atttypmod = -1;
        attrs.push_back(std::move(a));
    }
    return CreateTupleDesc(attrs);
}

// Build a HeapTuple from a vector of Datums + null flags for the given view.
// vector<bool> is bit-packed and has no .data(), so we copy to a plain array.
HeapTuple MakeStatsTuple(TupleDesc tupdesc, const std::vector<Datum>& values,
                         const std::vector<bool>& isnull) {
    std::unique_ptr<bool[]> bool_arr(new bool[values.size()]);
    for (std::size_t i = 0; i < isnull.size(); ++i) {
        bool_arr[i] = isnull[i];
    }
    return pgcpp::access::heap_form_tuple(tupdesc, values.data(), bool_arr.get());
}

// --- Per-view materializers ---

// Materialize pg_stat_database rows.
std::vector<HeapTuple> MaterializeDatabaseStats(TupleDesc tupdesc) {
    std::vector<HeapTuple> out;
    auto all_stats = GetStatsCollector().GetAllDatabaseStats();
    for (const auto& s : all_stats) {
        std::vector<Datum> values = {
            Int32GetDatum(static_cast<int32_t>(s.datid)),
            MakeTextDatum(s.datname),
            Int64GetDatum(s.num_commit),
            Int64GetDatum(s.num_rollback),
            Int64GetDatum(s.blks_read),
            Int64GetDatum(s.blks_hit),
            Int64GetDatum(s.tuples_returned),
            Int64GetDatum(s.tuples_fetched),
            Int64GetDatum(s.tuples_inserted),
            Int64GetDatum(s.tuples_updated),
            Int64GetDatum(s.tuples_deleted),
            Int64GetDatum(s.conflicts),
            Int64GetDatum(s.temp_files),
            Int64GetDatum(s.temp_bytes),
            Int64GetDatum(s.deadlocks),
        };
        std::vector<bool> isnull(values.size(), false);
        HeapTuple tup = MakeStatsTuple(tupdesc, values, isnull);
        out.push_back(tup);
    }
    return out;
}

// Materialize pg_stat_activity rows (single backend in pgcpp).
std::vector<HeapTuple> MaterializeActivity(TupleDesc tupdesc) {
    std::vector<HeapTuple> out;
    const auto& bi = GetStatsCollector().GetBackendInfo();
    std::vector<Datum> values = {
        Int32GetDatum(bi.pid),
        Int32GetDatum(static_cast<int32_t>(bi.datid)),
        MakeTextDatum(bi.datname),
        Int32GetDatum(static_cast<int32_t>(bi.userid)),
        MakeTextDatum(bi.application_name),
        MakeTextDatum(bi.state),
        MakeTextDatum(bi.query),
        Int64GetDatum(bi.backend_start),
        Int64GetDatum(bi.query_start),
        Int64GetDatum(bi.xact_start),
    };
    std::vector<bool> isnull(values.size(), false);
    HeapTuple tup = MakeStatsTuple(tupdesc, values, isnull);
    out.push_back(tup);
    return out;
}

// Materialize pg_stat_all_tables rows.
std::vector<HeapTuple> MaterializeAllTables(TupleDesc tupdesc) {
    std::vector<HeapTuple> out;
    auto all_stats = GetStatsCollector().GetAllTableStats();
    for (const auto& s : all_stats) {
        std::vector<Datum> values = {
            Int32GetDatum(static_cast<int32_t>(s.relid)),
            MakeTextDatum(s.schemaname),
            MakeTextDatum(s.relname),
            Int64GetDatum(s.seq_scan),
            Int64GetDatum(s.seq_tuples_read),
            Int64GetDatum(s.idx_scan),
            Int64GetDatum(s.idx_tuples_fetch),
            Int64GetDatum(s.n_tuples_ins),
            Int64GetDatum(s.n_tuples_upd),
            Int64GetDatum(s.n_tuples_del),
            Int64GetDatum(s.n_tuples_hot_upd),
            Int64GetDatum(s.n_live_tuples),
            Int64GetDatum(s.n_dead_tuples),
            Int64GetDatum(s.last_vacuum),
            Int64GetDatum(s.last_autovacuum),
            Int64GetDatum(s.last_analyze),
            Int64GetDatum(s.last_autoanalyze),
        };
        std::vector<bool> isnull(values.size(), false);
        HeapTuple tup = MakeStatsTuple(tupdesc, values, isnull);
        out.push_back(tup);
    }
    return out;
}

// Materialize pg_stat_all_indexes rows.
std::vector<HeapTuple> MaterializeAllIndexes(TupleDesc tupdesc) {
    std::vector<HeapTuple> out;
    auto all_stats = GetStatsCollector().GetAllIndexStats();
    for (const auto& s : all_stats) {
        std::vector<Datum> values = {
            Int32GetDatum(static_cast<int32_t>(s.relid)),
            MakeTextDatum(s.schemaname),
            MakeTextDatum(s.relname),
            Int32GetDatum(static_cast<int32_t>(s.tableoid)),
            Int64GetDatum(s.idx_scan),
            Int64GetDatum(s.idx_tuples_read),
            Int64GetDatum(s.idx_tuples_fetch),
        };
        std::vector<bool> isnull(values.size(), false);
        HeapTuple tup = MakeStatsTuple(tupdesc, values, isnull);
        out.push_back(tup);
    }
    return out;
}

}  // namespace

StatsScanDesc* StatsBeginScan(Oid relid) {
    if (!IsStatsView(relid)) {
        return nullptr;
    }
    auto* scan = pgcpp::nodes::makePallocNode<StatsScanDesc>();
    scan->view_oid = relid;
    scan->next_index = 0;

    TupleDesc tupdesc = BuildStatsTupleDesc(relid);
    if (tupdesc == nullptr) {
        return scan;
    }

    switch (relid) {
        case kPgStatDatabaseOid:
            scan->tuples = MaterializeDatabaseStats(tupdesc);
            break;
        case kPgStatActivityOid:
            scan->tuples = MaterializeActivity(tupdesc);
            break;
        case kPgStatAllTablesOid:
            scan->tuples = MaterializeAllTables(tupdesc);
            break;
        case kPgStatAllIndexesOid:
            scan->tuples = MaterializeAllIndexes(tupdesc);
            break;
        default:
            break;
    }
    return scan;
}

HeapTuple StatsGetNext(StatsScanDesc* scan) {
    if (scan == nullptr) {
        return nullptr;
    }
    if (scan->next_index >= scan->tuples.size()) {
        return nullptr;
    }
    return scan->tuples[scan->next_index++];
}

void StatsEndScan(StatsScanDesc* scan) {
    if (scan == nullptr) {
        return;
    }
    // Free the materialized tuples.
    for (HeapTuple tup : scan->tuples) {
        pgcpp::access::heap_freetuple(tup);
    }
    scan->tuples.clear();
    scan->next_index = 0;
    // The descriptor itself is palloc'd and freed by the memory context.
}

}  // namespace pgcpp::stats

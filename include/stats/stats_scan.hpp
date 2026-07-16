// stats_scan.hpp — Virtual scan for pg_stat_* views (P3-9).
//
// A StatsScanDesc holds a vector of pre-materialized HeapTuples produced
// from the StatisticsCollector. When the executor scans a pg_stat_* view,
// SeqScanState uses this descriptor instead of a HeapScanDesc.
//
// The scan is snapshot-free (stats are always current) and read-only.

#pragma once

#include <cstdint>
#include <vector>

#include "catalog/catalog.hpp"

namespace pgcpp::transaction {
struct HeapTupleData;
using HeapTuple = HeapTupleData*;
}  // namespace pgcpp::transaction

namespace pgcpp::stats {

// StatsScanDesc — a virtual scan descriptor for pg_stat_* views.
struct StatsScanDesc {
    pgcpp::catalog::Oid view_oid = 0;  // which pg_stat_* view
    std::vector<pgcpp::transaction::HeapTuple> tuples;
    std::size_t next_index = 0;  // next tuple to return
};

// Create a stats scan for the given view OID. Materializes all rows from
// the StatisticsCollector into the scan descriptor. Returns nullptr if the
// OID is not a stats view.
StatsScanDesc* StatsBeginScan(pgcpp::catalog::Oid relid);

// Fetch the next tuple from a stats scan. Returns nullptr when exhausted.
pgcpp::transaction::HeapTuple StatsGetNext(StatsScanDesc* scan);

// End a stats scan (frees the descriptor and its tuples).
void StatsEndScan(StatsScanDesc* scan);

}  // namespace pgcpp::stats

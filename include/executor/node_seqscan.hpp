// node_seqscan.h — Sequential scan node state.
#pragma once

#include "access/heapam.hpp"
#include "access/rel.hpp"
#include "executor/node_exec.hpp"
#include "stats/stats_scan.hpp"

namespace pgcpp::executor {

class SeqScanState : public PlanState {
public:
    SeqScanState(Plan* p, EState* s) : PlanState(p, s) {}

    pgcpp::access::Relation ss_relation = nullptr;
    pgcpp::access::HeapScanDesc ss_scanDesc = nullptr;
    TupleTableSlot* ss_ScanTupleSlot = nullptr;

    // P3-9: when the scanned relation is a pg_stat_* view, ss_stats_scan
    // holds the virtual scan descriptor and ss_scanDesc is null.
    pgcpp::stats::StatsScanDesc* ss_stats_scan = nullptr;
    int64_t ss_tuples_returned = 0;  // count for stats reporting

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;
};

}  // namespace pgcpp::executor

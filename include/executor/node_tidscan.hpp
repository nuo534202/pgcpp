// node_tidscan.h — TidScan node state (scan by specific TIDs).
//
// Fetches each tuple at the listed TIDs directly via heap_fetch_by_tid,
// applies the qual filter, and projects the target list.
#pragma once

#include "access/heapam.hpp"
#include "access/rel.hpp"
#include "executor/node_exec.hpp"

namespace pgcpp::executor {

class TidScanState : public PlanState {
public:
    TidScanState(Plan* p, EState* s) : PlanState(p, s) {}

    pgcpp::access::Relation ts_relation = nullptr;
    TupleTableSlot* ts_ScanTupleSlot = nullptr;
    size_t ts_tid_index = 0;  // next TID to fetch

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;
};

}  // namespace pgcpp::executor

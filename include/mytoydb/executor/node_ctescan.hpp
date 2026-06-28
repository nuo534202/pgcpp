// node_ctescan.h — CteScan node state (CTE reference).
//
// Caches the CTE's result on first scan. Subsequent scans of the
// same CTE replay the cached tuples. The CTE's plan is stored
// separately and shared across all CteScanStates referencing it.
#pragma once

#include <vector>

#include "mytoydb/executor/node_exec.hpp"

namespace mytoydb::executor {

// CteScanState — scan a CTE result.
// On first call, executes the CTE's subplan and caches all tuples.
// Returns cached tuples one by one.
class CteScanState : public PlanState {
public:
    CteScanState(Plan* p, EState* s) : PlanState(p, s) {}

    int cs_cte_id = 0;                       // CTE index
    std::vector<TupleTableSlot*> cs_tuples;  // cached CTE tuples
    bool cs_done = false;                    // CTE fully materialized?
    size_t cs_index = 0;                     // next tuple to return

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;
};

}  // namespace mytoydb::executor

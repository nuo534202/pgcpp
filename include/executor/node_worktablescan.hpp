// node_worktablescan.h — WorkTableScan node state.
//
// Reads tuples from the working table registered under wtParam on the EState
// by the parent RecursiveUnion. Used inside the recursive term of a
// WITH RECURSIVE query.
#pragma once

#include "executor/node_exec.hpp"

namespace pgcpp::executor {

class WorkTableScanState : public PlanState {
public:
    WorkTableScanState(Plan* p, EState* s) : PlanState(p, s) {}

    int wt_wtParam = 0;

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;
};

}  // namespace pgcpp::executor

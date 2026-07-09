// node_functionscan.h — FunctionScan node state (set-returning functions).
//
// Evaluates the plan's function expressions (FuncExpr nodes that return a
// set) and emits one row per value produced. Currently supports
// generate_series(int4, int4).
#pragma once

#include <vector>

#include "executor/node_exec.hpp"

namespace pgcpp::executor {

class FunctionScanState : public PlanState {
public:
    FunctionScanState(Plan* p, EState* s) : PlanState(p, s) {}

    // Buffered rows produced by the SRFs (evaluated lazily on first call).
    std::vector<TupleTableSlot*> fs_rows;
    bool fs_done = false;
    size_t fs_index = 0;

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;
};

}  // namespace pgcpp::executor

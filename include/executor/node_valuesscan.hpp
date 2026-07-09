// node_valuesscan.h — ValuesScan node state (scan a VALUES list).
//
// Each row in the plan's `rows` is a list of expression nodes. The executor
// evaluates each row's expressions to produce output tuples.
#pragma once

#include "executor/node_exec.hpp"

namespace pgcpp::executor {

class ValuesScanState : public PlanState {
public:
    ValuesScanState(Plan* p, EState* s) : PlanState(p, s) {}

    size_t vs_row_index = 0;  // next row to evaluate

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;
};

}  // namespace pgcpp::executor

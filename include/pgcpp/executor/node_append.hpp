// node_append.h — Append node state (UNION ALL).
//
// Iterates over multiple child plans sequentially. Each child is
// drained before moving to the next.
#pragma once

#include <vector>

#include "pgcpp/executor/node_exec.hpp"

namespace pgcpp::executor {

class AppendState : public PlanState {
public:
    AppendState(Plan* p, EState* s) : PlanState(p, s) {}

    std::vector<PlanState*> append_ps;  // child plan states
    int as_whichplan = 0;               // index of current child

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;
};

}  // namespace pgcpp::executor

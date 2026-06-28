// node_limit.h — Limit node state (LIMIT/OFFSET).
#pragma once

#include "pgcpp/executor/node_exec.hpp"

namespace pgcpp::executor {

class LimitState : public PlanState {
public:
    LimitState(Plan* p, EState* s) : PlanState(p, s) {}

    int64_t li_count = -1;  // remaining rows to return
    int64_t li_offset = 0;  // rows still to skip
    bool li_done = false;   // exhausted?

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;
};

}  // namespace pgcpp::executor

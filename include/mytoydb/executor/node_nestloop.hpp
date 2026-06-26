// node_nestloop.h — Nested-loop join node state.
#pragma once

#include "mytoydb/executor/node_exec.hpp"

namespace mytoydb::executor {

class NestLoopState : public PlanState {
public:
    NestLoopState(Plan* p, EState* s) : PlanState(p, s) {}

    mytoydb::parser::JoinType nl_jointype = mytoydb::parser::JoinType::kInner;
    TupleTableSlot* nl_OuterTupleSlot = nullptr;
    TupleTableSlot* nl_InnerTupleSlot = nullptr;
    bool nl_NeedNewOuter = true;
    bool nl_MatchedOuter = false;

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;
};

}  // namespace mytoydb::executor

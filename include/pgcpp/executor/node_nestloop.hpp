// node_nestloop.h — Nested-loop join node state.
#pragma once

#include "pgcpp/executor/node_exec.hpp"

namespace pgcpp::executor {

class NestLoopState : public PlanState {
public:
    NestLoopState(Plan* p, EState* s) : PlanState(p, s) {}

    pgcpp::parser::JoinType nl_jointype = pgcpp::parser::JoinType::kInner;
    TupleTableSlot* nl_OuterTupleSlot = nullptr;
    TupleTableSlot* nl_InnerTupleSlot = nullptr;
    bool nl_NeedNewOuter = true;
    bool nl_MatchedOuter = false;

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;
};

}  // namespace pgcpp::executor

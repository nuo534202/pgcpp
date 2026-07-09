// node_gather.h — Gather / GatherMerge node state.
//
// In PostgreSQL these nodes launch parallel workers and gather their output.
// pgcpp forbids std::thread and uses a fork-based multi-process model that is
// not wired into the executor, so the leader executes the child plan directly
// (the nworkers = 0 serial fallback). GatherMerge additionally sorts the
// child's output on the merge keys.
#pragma once

#include <memory>
#include <vector>

#include "executor/node_exec.hpp"
#include "utils/sort/tuplesort.hpp"

namespace pgcpp::executor {

class GatherState : public PlanState {
public:
    GatherState(Plan* p, EState* s) : PlanState(p, s) {}

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;
};

class GatherMergeState : public PlanState {
public:
    GatherMergeState(Plan* p, EState* s) : PlanState(p, s) {}

    std::unique_ptr<pgcpp::sort::TupleSort> tuplesort;
    bool sorted_done = false;

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;
};

}  // namespace pgcpp::executor

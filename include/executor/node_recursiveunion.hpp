// node_recursiveunion.h — RecursiveUnion node state (WITH RECURSIVE).
//
// lefttree is the seed term; righttree is the recursive term. Each iteration:
//   1. Populate the working table (registered on EState under wtParam) with
//      the new rows from the previous iteration.
//   2. Re-scan and drain the recursive term; new rows go into the result and
//      become the next working table.
// Iteration stops when the recursive term produces no new rows.
#pragma once

#include <vector>

#include "executor/node_exec.hpp"

namespace pgcpp::executor {

class RecursiveUnionState : public PlanState {
public:
    RecursiveUnionState(Plan* p, EState* s) : PlanState(p, s) {}

    int ru_wtParam = 0;
    // The full result accumulated so far.
    std::vector<TupleTableSlot*> ru_results;
    size_t ru_result_index = 0;
    bool ru_initialized = false;  // seed drained into results?
    bool ru_done = false;         // recursive fixpoint reached?

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;
};

}  // namespace pgcpp::executor

// node_sort.h — Sort node state with external merge sort (P1-3).
#pragma once

#include <cstdint>
#include <memory>

#include "executor/node_exec.hpp"
#include "utils/sort/tuplesort.hpp"

namespace pgcpp::executor {

class SortState : public PlanState {
public:
    SortState(Plan* p, EState* s) : PlanState(p, s) {}

    // External merge sort state (P1-3). Replaces the old in-memory
    // sorted_tuples vector. Owned by SortState, allocated in ExecInit.
    std::unique_ptr<pgcpp::sort::TupleSort> tuplesort;
    bool sorted_done = false;  // have we sorted?
    int64_t rows_skipped = 0;  // OFFSET bookkeeping
    int64_t rows_output = 0;   // LIMIT bookkeeping

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;
};

}  // namespace pgcpp::executor

// node_incrementalsort.h — IncrementalSort node state.
//
// Sorts by the full sort keys, exploiting an existing prefix sort. Reads runs
// of tuples sharing the same prefix values, fully sorts each run on the
// remaining keys, and emits the sorted runs in order.
#pragma once

#include <memory>
#include <vector>

#include "executor/node_exec.hpp"
#include "utils/sort/tuplesort.hpp"

namespace pgcpp::executor {

class IncrementalSortState : public PlanState {
public:
    IncrementalSortState(Plan* p, EState* s) : PlanState(p, s) {}

    std::unique_ptr<pgcpp::sort::TupleSort> group_sort;  // per-group sorter
    bool is_done = false;                                // all input consumed and emitted
    bool group_full = false;  // current group fully sorted & being drained
    // Prefix values of the current group (for detecting group boundaries).
    std::vector<pgcpp::types::Datum> cur_prefix_values;
    std::vector<bool> cur_prefix_nulls;
    bool first_tuple = true;  // no group started yet

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;
};

}  // namespace pgcpp::executor

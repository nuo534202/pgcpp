// node_sort.h — Sort node state with Top-N optimization.
#pragma once

#include <vector>

#include "executor/node_exec.hpp"

namespace pgcpp::executor {

class SortState : public PlanState {
public:
    SortState(Plan* p, EState* s) : PlanState(p, s) {}

    std::vector<TupleTableSlot*> sorted_tuples;  // all tuples collected
    bool sorted_done = false;                    // have we sorted?
    size_t output_index = 0;                     // next tuple to output

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;

private:
    void SortTuples();
};

}  // namespace pgcpp::executor

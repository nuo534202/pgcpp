// node_group.h — Group node state (GROUP BY without aggregates).
//
// Assumes the child produces tuples already sorted on groupColIdx columns
// so identical groups are adjacent. Emits the first tuple of each group;
// subsequent tuples in the same group are consumed without output.
#pragma once

#include <vector>

#include "executor/node_exec.hpp"

namespace pgcpp::executor {

class GroupState : public PlanState {
public:
    GroupState(Plan* p, EState* s) : PlanState(p, s) {}

    std::vector<int> groupColIdx;           // 1-based attr numbers of GROUP BY cols
    TupleTableSlot* first_tuple = nullptr;  // first tuple of the current group
    bool has_first = false;                 // has first_tuple been set for this group?

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;
};

}  // namespace pgcpp::executor

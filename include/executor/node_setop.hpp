// node_setop.h — SetOp node state (INTERSECT / EXCEPT).
//
// Takes a single sorted child (typically an Append of the two inputs with a
// flag column). Groups on colIdx columns, counts rows from each input (using
// the flag column), and applies the set operation:
//   INTERSECT DISTINCT: emit one row per group present in both inputs
//   INTERSECT ALL: emit min(leftCount, rightCount) copies per group
//   EXCEPT DISTINCT: emit one row per group in left but not in right
//   EXCEPT ALL: emit max(0, leftCount - rightCount) copies per group
#pragma once

#include <vector>

#include "executor/node_exec.hpp"

namespace pgcpp::executor {

class SetOpState : public PlanState {
public:
    SetOpState(Plan* p, EState* s) : PlanState(p, s) {}

    // Plan parameters (copied from SetOp plan in ExecInit).
    SetOp::Cmd cmd = SetOp::Cmd::kIntersect;
    bool all = false;
    std::vector<int> colIdx;  // 1-based attr numbers to compare on
    int flagColIdx = 0;       // 1-based attr number of the flag column
    int firstFlag = 0;        // flag value of the first (left) input

    // Per-group accumulators.
    int left_count = 0;                     // rows from left input in current group
    int right_count = 0;                    // rows from right input in current group
    TupleTableSlot* group_first = nullptr;  // first tuple of current group
    bool has_group = false;                 // is a group in progress?

    // Output state for the current group.
    int remaining_output = 0;  // rows left to emit for the current group

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;

private:
    // Compute how many rows to emit for the current group.
    int ComputeOutputCount() const;
    // Start a new group from the given child tuple.
    void StartNewGroup(TupleTableSlot* child_slot);
};

}  // namespace pgcpp::executor

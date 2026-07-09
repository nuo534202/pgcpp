// node_setop.cpp — SetOp node implementation (INTERSECT / EXCEPT).
//
// Takes a single sorted child (typically an Append of the two inputs with a
// flag column appended). The flag column is 0 for rows from the left input
// and 1 for rows from the right input. Groups on colIdx columns, counts rows
// from each input, and applies the set operation.
//
// The child must be sorted on colIdx columns so identical groups are adjacent.
// Within a group, rows from the same input are also adjacent (this is
// naturally the case when the child is an Append of two sorted subplans).
#include "executor/node_setop.hpp"

#include <algorithm>

#include "common/containers/node.hpp"
#include "executor/estate.hpp"
#include "executor/exec_expr.hpp"
#include "executor/exec_utils.hpp"
#include "executor/plannodes.hpp"
#include "executor/tupletable.hpp"
#include "types/datum.hpp"

namespace pgcpp::executor {

using pgcpp::nodes::destroyPallocNode;
using pgcpp::types::DatumGetInt32;

void SetOpState::ExecInit() {
    auto* setopplan = static_cast<SetOp*>(plan);
    cmd = setopplan->cmd;
    all = setopplan->all;
    colIdx = setopplan->colIdx;
    flagColIdx = setopplan->flagColIdx;
    firstFlag = setopplan->firstFlag;

    auto* result_desc = BuildTupleDescFromTargetList(plan->targetlist);
    ps_ResultTupleSlot = TupleTableSlot::Make(result_desc);
    state->es_tupleTable.push_back(ps_ResultTupleSlot);
    ps_ExprContext = CreateExprContext();
    if (leftps != nullptr && leftps->ps_ResultTupleSlot != nullptr) {
        ps_ExprContext->ecxt_scantuple = leftps->ps_ResultTupleSlot;
    }
    group_first = nullptr;
    has_group = false;
    left_count = 0;
    right_count = 0;
    remaining_output = 0;
}

int SetOpState::ComputeOutputCount() const {
    if (cmd == SetOp::Cmd::kIntersect) {
        if (all) {
            return std::min(left_count, right_count);
        }
        // DISTINCT: emit one row if both sides have at least one.
        return (left_count > 0 && right_count > 0) ? 1 : 0;
    } else {  // kExcept
        if (all) {
            return std::max(0, left_count - right_count);
        }
        // DISTINCT: emit one row if left has rows and right has none.
        return (left_count > 0 && right_count == 0) ? 1 : 0;
    }
}

void SetOpState::StartNewGroup(TupleTableSlot* child_slot) {
    if (group_first == nullptr) {
        group_first = TupleTableSlot::Make(child_slot->tts_tupleDescriptor);
    }
    group_first->StoreVirtual(child_slot->tts_values, child_slot->tts_isnull);
    has_group = true;
    left_count = 0;
    right_count = 0;

    // Determine which input this row came from.
    int flag_idx = flagColIdx - 1;
    if (flag_idx >= 0 && flag_idx < child_slot->Natts() && !child_slot->tts_isnull[flag_idx]) {
        int flag_val = DatumGetInt32(child_slot->tts_values[flag_idx]);
        if (flag_val == firstFlag) {
            left_count = 1;
        } else {
            right_count = 1;
        }
    } else {
        // No flag column — assume left.
        left_count = 1;
    }
    remaining_output = 0;
}

TupleTableSlot* SetOpState::ExecProcNode() {
    for (;;) {
        // If we have remaining output rows for the current group, emit them.
        if (remaining_output > 0) {
            remaining_output--;
            ResetExprContext(ps_ExprContext);
            int natts = ps_ResultTupleSlot->Natts();
            int src_natts = group_first->Natts();
            int ncopy = natts < src_natts ? natts : src_natts;
            for (int i = 0; i < ncopy; i++) {
                ps_ResultTupleSlot->tts_values[i] = group_first->tts_values[i];
                ps_ResultTupleSlot->tts_isnull[i] = group_first->tts_isnull[i];
            }
            ps_ResultTupleSlot->tts_nvalid = true;
            ps_ResultTupleSlot->tts_isempty = false;
            return ps_ResultTupleSlot;
        }

        // Read the next tuple from the child.
        TupleTableSlot* child_slot = leftps->ExecProcNode();
        if (child_slot == nullptr) {
            // End of input — flush the last group if it has pending output.
            if (has_group) {
                remaining_output = ComputeOutputCount();
                has_group = false;
                if (remaining_output > 0) {
                    remaining_output--;
                    ResetExprContext(ps_ExprContext);
                    int natts = ps_ResultTupleSlot->Natts();
                    int src_natts = group_first->Natts();
                    int ncopy = natts < src_natts ? natts : src_natts;
                    for (int i = 0; i < ncopy; i++) {
                        ps_ResultTupleSlot->tts_values[i] = group_first->tts_values[i];
                        ps_ResultTupleSlot->tts_isnull[i] = group_first->tts_isnull[i];
                    }
                    ps_ResultTupleSlot->tts_nvalid = true;
                    ps_ResultTupleSlot->tts_isempty = false;
                    return ps_ResultTupleSlot;
                }
            }
            return nullptr;
        }

        if (!has_group) {
            // First tuple — start a new group.
            StartNewGroup(child_slot);
            continue;
        }

        // Check if this tuple is in the same group.
        if (CompareTuplesOnAttrs(group_first, child_slot, colIdx) == 0) {
            // Same group — increment the appropriate counter.
            int flag_idx = flagColIdx - 1;
            if (flag_idx >= 0 && flag_idx < child_slot->Natts() &&
                !child_slot->tts_isnull[flag_idx]) {
                int flag_val = DatumGetInt32(child_slot->tts_values[flag_idx]);
                if (flag_val == firstFlag) {
                    left_count++;
                } else {
                    right_count++;
                }
            } else {
                left_count++;
            }
            continue;
        }

        // Different group — compute output for the completed group.
        remaining_output = ComputeOutputCount();
        has_group = false;
        if (remaining_output > 0) {
            // Emit the first row now, save the new group for next iteration.
            remaining_output--;
            ResetExprContext(ps_ExprContext);
            int natts = ps_ResultTupleSlot->Natts();
            int src_natts = group_first->Natts();
            int ncopy = natts < src_natts ? natts : src_natts;
            for (int i = 0; i < ncopy; i++) {
                ps_ResultTupleSlot->tts_values[i] = group_first->tts_values[i];
                ps_ResultTupleSlot->tts_isnull[i] = group_first->tts_isnull[i];
            }
            ps_ResultTupleSlot->tts_nvalid = true;
            ps_ResultTupleSlot->tts_isempty = false;

            // Start the new group with this tuple.
            StartNewGroup(child_slot);
            return ps_ResultTupleSlot;
        }

        // No output for the completed group — start the new group.
        StartNewGroup(child_slot);
    }
}

void SetOpState::ExecEnd() {
    if (group_first != nullptr) {
        destroyPallocNode(group_first);
        group_first = nullptr;
    }

    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
}

void SetOpState::ExecReScan() {
    if (group_first != nullptr) {
        destroyPallocNode(group_first);
        group_first = nullptr;
    }
    has_group = false;
    left_count = 0;
    right_count = 0;
    remaining_output = 0;
    if (leftps != nullptr)
        leftps->ExecReScan();
}

}  // namespace pgcpp::executor

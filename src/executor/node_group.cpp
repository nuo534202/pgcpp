// node_group.cpp — Group node implementation (GROUP BY without aggregates).
//
// Assumes the child produces tuples already sorted on groupColIdx columns.
// Emits the first tuple of each group; subsequent tuples in the same group
// are skipped. This is the simple-form analogue of a sorted Agg with no
// aggregate functions in the target list.
//
// Streaming approach: the first tuple of each group is returned immediately
// when read. Subsequent tuples with the same group key are consumed without
// output. When a tuple with a different key arrives, it becomes the first
// of a new group and is returned.
#include "executor/node_group.hpp"

#include "common/containers/node.hpp"
#include "executor/estate.hpp"
#include "executor/exec_expr.hpp"
#include "executor/exec_utils.hpp"
#include "executor/plannodes.hpp"
#include "executor/tupletable.hpp"

namespace pgcpp::executor {

using pgcpp::nodes::destroyPallocNode;

void GroupState::ExecInit() {
    auto* groupplan = static_cast<Group*>(plan);
    groupColIdx = groupplan->groupColIdx;

    auto* result_desc = BuildTupleDescFromTargetList(plan->targetlist);
    ps_ResultTupleSlot = TupleTableSlot::Make(result_desc);
    state->es_tupleTable.push_back(ps_ResultTupleSlot);
    ps_ExprContext = CreateExprContext();
    if (leftps != nullptr && leftps->ps_ResultTupleSlot != nullptr) {
        ps_ExprContext->ecxt_scantuple = leftps->ps_ResultTupleSlot;
    }
    first_tuple = nullptr;
    has_first = false;
}

TupleTableSlot* GroupState::ExecProcNode() {
    for (;;) {
        TupleTableSlot* child_slot = leftps->ExecProcNode();
        if (child_slot == nullptr) {
            // End of input — the last group's first tuple was already returned.
            return nullptr;
        }

        if (!has_first) {
            // First tuple ever — save as current group's first and return it.
            first_tuple = TupleTableSlot::Make(child_slot->tts_tupleDescriptor);
            first_tuple->StoreVirtual(child_slot->tts_values, child_slot->tts_isnull);
            has_first = true;

            // Project into result slot.
            ResetExprContext(ps_ExprContext);
            int natts = ps_ResultTupleSlot->Natts();
            int src_natts = child_slot->Natts();
            int ncopy = natts < src_natts ? natts : src_natts;
            for (int i = 0; i < ncopy; i++) {
                ps_ResultTupleSlot->tts_values[i] = child_slot->tts_values[i];
                ps_ResultTupleSlot->tts_isnull[i] = child_slot->tts_isnull[i];
            }
            ps_ResultTupleSlot->tts_nvalid = true;
            ps_ResultTupleSlot->tts_isempty = false;
            return ps_ResultTupleSlot;
        }

        // Compare with the current group's first tuple.
        if (CompareTuplesOnAttrs(first_tuple, child_slot, groupColIdx) == 0) {
            // Same group — skip this tuple.
            continue;
        }

        // Different group — save this as the new group's first and return it.
        first_tuple->StoreVirtual(child_slot->tts_values, child_slot->tts_isnull);

        ResetExprContext(ps_ExprContext);
        int natts = ps_ResultTupleSlot->Natts();
        int src_natts = child_slot->Natts();
        int ncopy = natts < src_natts ? natts : src_natts;
        for (int i = 0; i < ncopy; i++) {
            ps_ResultTupleSlot->tts_values[i] = child_slot->tts_values[i];
            ps_ResultTupleSlot->tts_isnull[i] = child_slot->tts_isnull[i];
        }
        ps_ResultTupleSlot->tts_nvalid = true;
        ps_ResultTupleSlot->tts_isempty = false;
        return ps_ResultTupleSlot;
    }
}

void GroupState::ExecEnd() {
    if (first_tuple != nullptr) {
        destroyPallocNode(first_tuple);
        first_tuple = nullptr;
    }

    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
}

void GroupState::ExecReScan() {
    if (first_tuple != nullptr) {
        destroyPallocNode(first_tuple);
        first_tuple = nullptr;
    }
    has_first = false;
    if (leftps != nullptr)
        leftps->ExecReScan();
}

}  // namespace pgcpp::executor

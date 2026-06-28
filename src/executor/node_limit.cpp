// node_limit.cpp — Limit node implementation (LIMIT/OFFSET).
//
// Wraps a child plan and returns at most `limit_count` rows after
// skipping `offset_count` rows. limit_count < 0 means no limit.
#include "pgcpp/executor/node_limit.hpp"

#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/executor/estate.hpp"
#include "pgcpp/executor/exec_expr.hpp"
#include "pgcpp/executor/exec_utils.hpp"
#include "pgcpp/executor/plannodes.hpp"
#include "pgcpp/executor/tupletable.hpp"

namespace pgcpp::executor {

using pgcpp::nodes::makePallocNode;

void LimitState::ExecInit() {
    auto* limitplan = static_cast<Limit*>(plan);
    li_count = limitplan->limit_count;
    li_offset = limitplan->offset_count;
    li_done = false;

    // Build the result slot from the target list (same shape as child).
    auto* result_desc = BuildTupleDescFromTargetList(plan->targetlist);
    ps_ResultTupleSlot = TupleTableSlot::Make(result_desc);
    state->es_tupleTable.push_back(ps_ResultTupleSlot);
    ps_ExprContext = CreateExprContext();
    if (leftps != nullptr && leftps->ps_ResultTupleSlot != nullptr) {
        ps_ExprContext->ecxt_scantuple = leftps->ps_ResultTupleSlot;
    }
}

TupleTableSlot* LimitState::ExecProcNode() {
    if (li_done)
        return nullptr;

    // Skip OFFSET rows.
    while (li_offset > 0) {
        TupleTableSlot* slot = leftps->ExecProcNode();
        if (slot == nullptr) {
            li_done = true;
            return nullptr;
        }
        li_offset--;
    }

    // Check LIMIT.
    if (li_count == 0) {
        li_done = true;
        return nullptr;
    }

    TupleTableSlot* child_slot = leftps->ExecProcNode();
    if (child_slot == nullptr) {
        li_done = true;
        return nullptr;
    }

    // Decrement remaining count.
    if (li_count > 0)
        li_count--;

    // Pass through the child's values into the result slot.
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

void LimitState::ExecEnd() {
    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
}

void LimitState::ExecReScan() {
    li_count = static_cast<Limit*>(plan)->limit_count;
    li_offset = static_cast<Limit*>(plan)->offset_count;
    li_done = false;
    if (leftps != nullptr)
        leftps->ExecReScan();
}

}  // namespace pgcpp::executor

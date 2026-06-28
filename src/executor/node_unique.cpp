// node_unique.cpp — Unique node implementation (SELECT DISTINCT).
//
// Deduplicates a sorted input. Assumes the child produces tuples
// already sorted on uniq_colIdx columns so duplicates are adjacent.
// Returns a tuple only when it differs from the previously returned one.
#include "pgcpp/executor/node_unique.hpp"

#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/executor/estate.hpp"
#include "pgcpp/executor/exec_expr.hpp"
#include "pgcpp/executor/exec_utils.hpp"
#include "pgcpp/executor/plannodes.hpp"
#include "pgcpp/executor/tupletable.hpp"

namespace pgcpp::executor {

using pgcpp::nodes::destroyPallocNode;
using pgcpp::nodes::makePallocNode;

void UniqueState::ExecInit() {
    auto* uniqueplan = static_cast<Unique*>(plan);
    uniq_colIdx = uniqueplan->uniq_colIdx;

    auto* result_desc = BuildTupleDescFromTargetList(plan->targetlist);
    ps_ResultTupleSlot = TupleTableSlot::Make(result_desc);
    state->es_tupleTable.push_back(ps_ResultTupleSlot);
    ps_ExprContext = CreateExprContext();
    if (leftps != nullptr && leftps->ps_ResultTupleSlot != nullptr) {
        ps_ExprContext->ecxt_scantuple = leftps->ps_ResultTupleSlot;
    }
    last_tuple = nullptr;
    has_last = false;
}

TupleTableSlot* UniqueState::ExecProcNode() {
    for (;;) {
        TupleTableSlot* child_slot = leftps->ExecProcNode();
        if (child_slot == nullptr)
            return nullptr;

        // If we have a previous tuple, check if this one is a duplicate.
        if (has_last && last_tuple != nullptr) {
            if (CompareTuplesOnAttrs(last_tuple, child_slot, uniq_colIdx) == 0) {
                continue;  // duplicate — skip
            }
        }

        // Save this tuple as the new "last" for future comparison.
        // Copy it so the child slot can be reused.
        if (last_tuple != nullptr)
            destroyPallocNode(last_tuple);
        last_tuple = TupleTableSlot::Make(child_slot->tts_tupleDescriptor);
        last_tuple->StoreVirtual(child_slot->tts_values, child_slot->tts_isnull);
        has_last = true;

        // Pass through to result slot.
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

void UniqueState::ExecEnd() {
    if (last_tuple != nullptr) {
        destroyPallocNode(last_tuple);
        last_tuple = nullptr;
    }

    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
}

void UniqueState::ExecReScan() {
    if (last_tuple != nullptr) {
        destroyPallocNode(last_tuple);
        last_tuple = nullptr;
    }
    has_last = false;
    if (leftps != nullptr)
        leftps->ExecReScan();
}

}  // namespace pgcpp::executor

// node_material.cpp — Material node implementation (cache child output).
//
// On first scan, drains the child into an in-memory tuple store.
// Subsequent rescans replay the stored tuples without re-executing
// the child.
#include "pgcpp/executor/node_material.hpp"

#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/executor/estate.hpp"
#include "pgcpp/executor/exec_expr.hpp"
#include "pgcpp/executor/exec_utils.hpp"
#include "pgcpp/executor/plannodes.hpp"
#include "pgcpp/executor/tupletable.hpp"

namespace mytoydb::executor {

using mytoydb::nodes::destroyPallocNode;
using mytoydb::nodes::makePallocNode;

void MaterialState::ExecInit() {
    auto* result_desc = BuildTupleDescFromTargetList(plan->targetlist);
    ps_ResultTupleSlot = TupleTableSlot::Make(result_desc);
    state->es_tupleTable.push_back(ps_ResultTupleSlot);
    ps_ExprContext = CreateExprContext();
    if (leftps != nullptr && leftps->ps_ResultTupleSlot != nullptr) {
        ps_ExprContext->ecxt_scantuple = leftps->ps_ResultTupleSlot;
    }
    mat_done = false;
    mat_index = 0;
}

TupleTableSlot* MaterialState::ExecProcNode() {
    if (!mat_done) {
        // Phase 1: drain child into cache.
        for (;;) {
            TupleTableSlot* child_slot = leftps->ExecProcNode();
            if (child_slot == nullptr)
                break;
            // Copy the child slot.
            TupleTableSlot* copy = TupleTableSlot::Make(child_slot->tts_tupleDescriptor);
            copy->StoreVirtual(child_slot->tts_values, child_slot->tts_isnull);
            mat_tuples.push_back(copy);
        }
        mat_done = true;
        mat_index = 0;
    }

    // Phase 2: replay cached tuples.
    if (mat_index >= mat_tuples.size())
        return nullptr;

    TupleTableSlot* src = mat_tuples[mat_index++];
    ResetExprContext(ps_ExprContext);
    int natts = ps_ResultTupleSlot->Natts();
    int src_natts = src->Natts();
    int ncopy = natts < src_natts ? natts : src_natts;
    for (int i = 0; i < ncopy; i++) {
        ps_ResultTupleSlot->tts_values[i] = src->tts_values[i];
        ps_ResultTupleSlot->tts_isnull[i] = src->tts_isnull[i];
    }
    ps_ResultTupleSlot->tts_nvalid = true;
    ps_ResultTupleSlot->tts_isempty = false;
    return ps_ResultTupleSlot;
}

void MaterialState::ExecEnd() {
    for (TupleTableSlot* slot : mat_tuples) {
        destroyPallocNode(slot);
    }
    mat_tuples.clear();

    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
}

void MaterialState::ExecReScan() {
    // On rescan, just reset the index to replay cached tuples.
    mat_index = 0;
}

}  // namespace mytoydb::executor

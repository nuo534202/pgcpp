// node_ctescan.cpp — CteScan node implementation (CTE reference).
//
// On first scan, the CTE's subplan is executed and all tuples are
// cached. Subsequent calls return cached tuples one by one.
// Rescan replays the cache from the beginning.
//
// Note: This implementation does not share the CTE cache across
// multiple CteScanStates referencing the same CTE — each CteScan
// independently materializes its copy. A future optimization could
// share the cache via a CTE state map on the EState.
#include "mytoydb/executor/node_ctescan.hpp"

#include "mytoydb/common/containers/node.hpp"
#include "mytoydb/executor/estate.hpp"
#include "mytoydb/executor/exec_expr.hpp"
#include "mytoydb/executor/exec_utils.hpp"
#include "mytoydb/executor/plannodes.hpp"
#include "mytoydb/executor/tupletable.hpp"

namespace mytoydb::executor {

using mytoydb::nodes::destroyPallocNode;
using mytoydb::nodes::makePallocNode;

void CteScanState::ExecInit() {
    auto* cteplan = static_cast<CteScan*>(plan);
    cs_cte_id = cteplan->cte_id;

    // Build result slot from target list.
    auto* result_desc = BuildTupleDescFromTargetList(plan->targetlist);
    ps_ResultTupleSlot = TupleTableSlot::Make(result_desc);
    state->es_tupleTable.push_back(ps_ResultTupleSlot);
    ps_ExprContext = CreateExprContext();

    cs_done = false;
    cs_index = 0;
}

TupleTableSlot* CteScanState::ExecProcNode() {
    if (!cs_done) {
        // Phase 1: execute the CTE's subplan (lefttree) and cache all tuples.
        if (leftps != nullptr) {
            for (;;) {
                TupleTableSlot* child_slot = leftps->ExecProcNode();
                if (child_slot == nullptr)
                    break;
                TupleTableSlot* copy = TupleTableSlot::Make(child_slot->tts_tupleDescriptor);
                copy->StoreVirtual(child_slot->tts_values, child_slot->tts_isnull);
                cs_tuples.push_back(copy);
            }
        }
        cs_done = true;
        cs_index = 0;
    }

    // Phase 2: return cached tuples.
    if (cs_index >= cs_tuples.size())
        return nullptr;

    TupleTableSlot* src = cs_tuples[cs_index++];
    ResetExprContext(ps_ExprContext);
    ps_ExprContext->ecxt_scantuple = src;

    // Evaluate qual if present.
    if (plan->qual != nullptr && !ExecQual(plan->qual, ps_ExprContext))
        return ExecProcNode();  // skip non-matching tuple

    // Project into result slot.
    ExecProject(plan->targetlist, ps_ExprContext, ps_ResultTupleSlot);
    return ps_ResultTupleSlot;
}

void CteScanState::ExecEnd() {
    for (TupleTableSlot* slot : cs_tuples) {
        destroyPallocNode(slot);
    }
    cs_tuples.clear();

    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
}

void CteScanState::ExecReScan() {
    // Just reset the index — the CTE is already materialized.
    cs_index = 0;
}

}  // namespace mytoydb::executor

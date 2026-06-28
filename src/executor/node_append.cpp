// node_append.cpp — Append node implementation (UNION ALL).
//
// Iterates over multiple child plans sequentially. Each child is
// fully drained before moving to the next. All children must
// produce the same column shape.
#include "mytoydb/executor/node_append.hpp"

#include "mytoydb/common/containers/node.hpp"
#include "mytoydb/executor/estate.hpp"
#include "mytoydb/executor/exec_expr.hpp"
#include "mytoydb/executor/exec_utils.hpp"
#include "mytoydb/executor/plannodes.hpp"
#include "mytoydb/executor/tupletable.hpp"

namespace mytoydb::executor {

using mytoydb::nodes::makePallocNode;

void AppendState::ExecInit() {
    auto* appendplan = static_cast<Append*>(plan);

    // Build the result slot from the first child's target list
    // (or the Append's own target list if available).
    auto* result_desc = BuildTupleDescFromTargetList(plan->targetlist);
    ps_ResultTupleSlot = TupleTableSlot::Make(result_desc);
    state->es_tupleTable.push_back(ps_ResultTupleSlot);
    ps_ExprContext = CreateExprContext();

    // Initialize child plan states for each append plan.
    // (Children are NOT in lefttree/righttree — they're in append_plans.
    //  ExecInitNode handles lefttree/righttree, so we must handle
    //  append_plans children ourselves here.)
    for (Plan* child_plan : appendplan->append_plans) {
        if (child_plan != nullptr) {
            PlanState* child_ps = ExecInitNode(child_plan, state);
            append_ps.push_back(child_ps);
        }
    }
    as_whichplan = 0;
}

TupleTableSlot* AppendState::ExecProcNode() {
    for (;;) {
        if (as_whichplan >= static_cast<int>(append_ps.size()))
            return nullptr;

        PlanState* child = append_ps[as_whichplan];
        TupleTableSlot* child_slot = child->ExecProcNode();

        if (child_slot != nullptr) {
            // Pass through child values into result slot.
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

        // Current child exhausted — move to next.
        as_whichplan++;
    }
}

void AppendState::ExecEnd() {
    // End each child (note: ExecEndNode would also destroyPallocNode
    // the child, but our children were created by ExecInitNode and need
    // the same teardown).
    for (PlanState* child : append_ps) {
        ExecEndNode(child);
    }
    append_ps.clear();

    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
}

void AppendState::ExecReScan() {
    as_whichplan = 0;
    for (PlanState* child : append_ps) {
        if (child != nullptr)
            child->ExecReScan();
    }
}

}  // namespace mytoydb::executor

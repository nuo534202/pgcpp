// node_subqueryscan.cpp — SubqueryScan node implementation (FROM subquery).
//
// Wraps a subquery's plan (stored as lefttree) and projects its
// output through the parent query's target list. The child plan
// is the subquery's plan, already initialized by ExecInitNode.
#include "mytoydb/executor/node_subqueryscan.hpp"

#include "mytoydb/common/containers/node.hpp"
#include "mytoydb/executor/estate.hpp"
#include "mytoydb/executor/exec_expr.hpp"
#include "mytoydb/executor/exec_utils.hpp"
#include "mytoydb/executor/plannodes.hpp"
#include "mytoydb/executor/tupletable.hpp"

namespace mytoydb::executor {

using mytoydb::nodes::makePallocNode;

void SubqueryScanState::ExecInit() {
    auto* subplan = static_cast<SubqueryScan*>(plan);

    // Build result slot from target list.
    auto* result_desc = BuildTupleDescFromTargetList(plan->targetlist);
    ps_ResultTupleSlot = TupleTableSlot::Make(result_desc);
    state->es_tupleTable.push_back(ps_ResultTupleSlot);

    // The child plan (lefttree) is already initialized by ExecInitNode.
    // Use the child's result slot as our scan tuple.
    if (leftps != nullptr) {
        ss_ScanTupleSlot = leftps->ps_ResultTupleSlot;
    }

    ps_ExprContext = CreateExprContext();
    ps_ExprContext->ecxt_scantuple = ss_ScanTupleSlot;
}

TupleTableSlot* SubqueryScanState::ExecProcNode() {
    for (;;) {
        TupleTableSlot* child_slot = leftps->ExecProcNode();
        if (child_slot == nullptr)
            return nullptr;

        // Set up the expression context with the child's tuple.
        ps_ExprContext->ecxt_scantuple = child_slot;
        ResetExprContext(ps_ExprContext);

        // Evaluate the qual (WHERE clause) if present.
        if (plan->qual != nullptr && !ExecQual(plan->qual, ps_ExprContext))
            continue;

        // Project the target list into the result slot.
        ExecProject(plan->targetlist, ps_ExprContext, ps_ResultTupleSlot);
        return ps_ResultTupleSlot;
    }
}

void SubqueryScanState::ExecEnd() {
    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
}

void SubqueryScanState::ExecReScan() {
    if (leftps != nullptr)
        leftps->ExecReScan();
}

}  // namespace mytoydb::executor

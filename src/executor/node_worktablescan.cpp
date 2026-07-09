// node_worktablescan.cpp — WorkTableScan node implementation.
//
// Converted from PostgreSQL 15's src/backend/execProcnode/nodeWorktablescan.c.
//
// Reads tuples from the working table registered under wtParam on the EState
// by the parent RecursiveUnion. Used inside the recursive term of a
// WITH RECURSIVE query.
#include "executor/node_worktablescan.hpp"

#include "common/containers/node.hpp"
#include "executor/estate.hpp"
#include "executor/exec_expr.hpp"
#include "executor/exec_utils.hpp"
#include "executor/plannodes.hpp"
#include "executor/tupletable.hpp"

namespace pgcpp::executor {

void WorkTableScanState::ExecInit() {
    auto* wtplan = static_cast<WorkTableScan*>(plan);
    wt_wtParam = wtplan->wtParam;

    // Build the result slot from the target list.
    auto* result_desc = BuildTupleDescFromTargetList(plan->targetlist);
    ps_ResultTupleSlot = TupleTableSlot::Make(result_desc);
    state->es_tupleTable.push_back(ps_ResultTupleSlot);
    ps_ExprContext = CreateExprContext();
}

TupleTableSlot* WorkTableScanState::ExecProcNode() {
    auto it = state->es_worktables.find(wt_wtParam);
    if (it == state->es_worktables.end() || it->second == nullptr)
        return nullptr;
    WorkTableState* wt = it->second;

    if (wt->index >= wt->tuples.size())
        return nullptr;

    TupleTableSlot* src = wt->tuples[wt->index++];
    ResetExprContext(ps_ExprContext);
    ps_ExprContext->ecxt_scantuple = src;

    // Evaluate qual if present.
    if (plan->qual != nullptr && !ExecQual(plan->qual, ps_ExprContext))
        return ExecProcNode();  // skip non-matching tuple

    // Project into result slot.
    ExecProject(plan->targetlist, ps_ExprContext, ps_ResultTupleSlot);
    return ps_ResultTupleSlot;
}

void WorkTableScanState::ExecEnd() {
    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
}

void WorkTableScanState::ExecReScan() {
    // Reset the working table's read position so the next iteration of the
    // recursive term re-reads from the start.
    auto it = state->es_worktables.find(wt_wtParam);
    if (it != state->es_worktables.end() && it->second != nullptr)
        it->second->index = 0;
}

}  // namespace pgcpp::executor

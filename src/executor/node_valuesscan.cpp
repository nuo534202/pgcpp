// node_valuesscan.cpp — ValuesScan node implementation (scan a VALUES list).
//
// Each row in the plan's `rows` is a list of expression nodes (one per output
// column). The executor evaluates each row's expressions to produce output
// tuples, applies the qual filter, and projects the target list.
#include "executor/node_valuesscan.hpp"

#include "common/containers/node.hpp"
#include "executor/estate.hpp"
#include "executor/exec_expr.hpp"
#include "executor/exec_utils.hpp"
#include "executor/plannodes.hpp"
#include "executor/tupletable.hpp"

namespace pgcpp::executor {

using pgcpp::nodes::makePallocNode;

void ValuesScanState::ExecInit() {
    auto* result_desc = BuildTupleDescFromTargetList(plan->targetlist);
    ps_ResultTupleSlot = TupleTableSlot::Make(result_desc);
    state->es_tupleTable.push_back(ps_ResultTupleSlot);
    ps_ExprContext = CreateExprContext();
    vs_row_index = 0;
}

TupleTableSlot* ValuesScanState::ExecProcNode() {
    auto* vsplan = static_cast<ValuesScan*>(plan);

    for (; vs_row_index < vsplan->rows.size(); vs_row_index++) {
        // Copy the row's expression list so advancing the index is safe.
        auto row_exprs = vsplan->rows[vs_row_index];
        ResetExprContext(ps_ExprContext);

        // Build a virtual scan tuple from the row's expressions by writing
        // directly into the result slot's value arrays (ValuesScan has no
        // separate relation). Note: std::vector<bool> is bit-packed and its
        // data() does not yield bool*, so we bypass StoreVirtual and write
        // the arrays directly.
        int natts = static_cast<int>(row_exprs.size());
        int slot_natts = ps_ResultTupleSlot->Natts();
        for (int i = 0; i < natts && i < slot_natts; i++) {
            bool arg_null = false;
            ps_ResultTupleSlot->tts_values[i] =
                ExecEvalExpr(row_exprs[i], ps_ExprContext, &arg_null);
            ps_ResultTupleSlot->tts_isnull[i] = arg_null;
        }
        ps_ResultTupleSlot->tts_nvalid = true;
        ps_ResultTupleSlot->tts_isempty = false;
        ps_ResultTupleSlot->tts_tuple = nullptr;
        ps_ResultTupleSlot->tts_shouldFree = false;
        ps_ExprContext->ecxt_scantuple = ps_ResultTupleSlot;

        // Evaluate the qual (WHERE clause) if present.
        if (plan->qual != nullptr && !ExecQual(plan->qual, ps_ExprContext)) {
            continue;  // row doesn't pass the filter
        }

        // Advance past this row before returning (so the next call continues).
        vs_row_index++;

        // Project the target list into the result slot.
        ExecProject(plan->targetlist, ps_ExprContext, ps_ResultTupleSlot);
        return ps_ResultTupleSlot;
    }
    return nullptr;  // no more rows
}

void ValuesScanState::ExecEnd() {
    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
}

void ValuesScanState::ExecReScan() {
    vs_row_index = 0;
}

}  // namespace pgcpp::executor

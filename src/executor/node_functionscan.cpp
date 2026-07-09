// node_functionscan.cpp — FunctionScan node implementation.
//
// Evaluates the plan's set-returning function expressions and emits one row
// per value produced. Currently supports generate_series(int4, int4), which
// produces int4 values from start to end inclusive.
#include "executor/node_functionscan.hpp"

#include "catalog/catalog.hpp"
#include "catalog/pg_proc.hpp"
#include "common/containers/node.hpp"
#include "executor/estate.hpp"
#include "executor/exec_expr.hpp"
#include "executor/exec_utils.hpp"
#include "executor/plannodes.hpp"
#include "executor/tupletable.hpp"
#include "parser/primnodes.hpp"
#include "types/datum.hpp"

namespace pgcpp::executor {

using pgcpp::catalog::GetCatalog;
using pgcpp::nodes::destroyPallocNode;
using pgcpp::parser::FuncExpr;
using pgcpp::parser::Node;
using pgcpp::types::Datum;
using pgcpp::types::DatumGetInt32;
using pgcpp::types::Int32GetDatum;

namespace {

// Evaluate a set-returning function, appending one int4 row per value to
// `rows`. Returns true if the function was recognized as an SRF.
bool EvalSRF(Node* fn_expr, ExprContext* econtext, TupleTableSlot* proto_slot,
             std::vector<TupleTableSlot*>& rows) {
    if (fn_expr->GetTag() != pgcpp::nodes::NodeTag::kFuncExpr) {
        return false;
    }
    auto* fn = static_cast<FuncExpr*>(fn_expr);
    const auto* proc = GetCatalog()->GetProcByOid(fn->funcid);
    if (proc == nullptr) {
        return false;
    }

    if (proc->proname == "generate_series") {
        // Evaluate the two int4 arguments.
        bool a_null = false, b_null = false;
        Datum a = ExecEvalExpr(fn->args[0], econtext, &a_null);
        Datum b = ExecEvalExpr(fn->args[1], econtext, &b_null);
        if (a_null || b_null) {
            return true;
        }
        int32_t start = DatumGetInt32(a);
        int32_t end = DatumGetInt32(b);
        for (int32_t v = start; v <= end; v++) {
            TupleTableSlot* slot = TupleTableSlot::Make(proto_slot->tts_tupleDescriptor);
            Datum val = Int32GetDatum(v);
            bool isnull = false;
            slot->StoreVirtual(&val, &isnull);
            rows.push_back(slot);
        }
        return true;
    }
    return false;
}

}  // namespace

void FunctionScanState::ExecInit() {
    auto* result_desc = BuildTupleDescFromTargetList(plan->targetlist);
    ps_ResultTupleSlot = TupleTableSlot::Make(result_desc);
    state->es_tupleTable.push_back(ps_ResultTupleSlot);
    ps_ExprContext = CreateExprContext();
    fs_done = false;
    fs_index = 0;
}

TupleTableSlot* FunctionScanState::ExecProcNode() {
    auto* fsplan = static_cast<FunctionScan*>(plan);

    if (!fs_done) {
        // Lazily evaluate all SRFs and buffer their rows.
        for (Node* fn_expr : fsplan->functions) {
            EvalSRF(fn_expr, ps_ExprContext, ps_ResultTupleSlot, fs_rows);
        }
        fs_done = true;
        fs_index = 0;
    }

    for (; fs_index < fs_rows.size(); fs_index++) {
        TupleTableSlot* src = fs_rows[fs_index];
        ResetExprContext(ps_ExprContext);
        ps_ExprContext->ecxt_scantuple = src;

        if (plan->qual != nullptr && !ExecQual(plan->qual, ps_ExprContext)) {
            continue;
        }
        fs_index++;  // advance before returning so the next call continues
        ExecProject(plan->targetlist, ps_ExprContext, ps_ResultTupleSlot);
        return ps_ResultTupleSlot;
    }
    return nullptr;
}

void FunctionScanState::ExecEnd() {
    for (TupleTableSlot* slot : fs_rows) {
        destroyPallocNode(slot);
    }
    fs_rows.clear();
    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
}

void FunctionScanState::ExecReScan() {
    fs_index = 0;
}

}  // namespace pgcpp::executor

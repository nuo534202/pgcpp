// node_projectset.cpp — ProjectSet node implementation.
//
// Projects a target list containing set-returning functions. For each input
// tuple (from lefttree), SRF target entries produce multiple values; the node
// returns one output row per SRF value (non-SRF entries repeat their value).
// When the target list has multiple SRFs, shorter SRFs are padded with NULLs
// once exhausted (PostgreSQL semantics).
#include "executor/node_projectset.hpp"

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
using pgcpp::nodes::NodeTag;
using pgcpp::parser::FuncExpr;
using pgcpp::parser::Node;
using pgcpp::parser::TargetEntry;
using pgcpp::types::Datum;
using pgcpp::types::DatumGetInt32;
using pgcpp::types::Int32GetDatum;

namespace {

// Evaluate a set-returning function into a value list. Returns true if the
// expression is a recognized SRF.
bool EvalSRFValues(Node* expr, ExprContext* econtext, std::vector<Datum>& values,
                   std::vector<bool>& isnull) {
    if (expr->GetTag() != NodeTag::kFuncExpr) {
        return false;
    }
    auto* fn = static_cast<FuncExpr*>(expr);
    const auto* proc = GetCatalog()->GetProcByOid(fn->funcid);
    if (proc == nullptr) {
        return false;
    }
    if (proc->proname == "generate_series") {
        bool a_null = false, b_null = false;
        Datum a = ExecEvalExpr(fn->args[0], econtext, &a_null);
        Datum b = ExecEvalExpr(fn->args[1], econtext, &b_null);
        if (a_null || b_null) {
            return true;
        }
        int32_t start = DatumGetInt32(a);
        int32_t end = DatumGetInt32(b);
        for (int32_t v = start; v <= end; v++) {
            values.push_back(Int32GetDatum(v));
            isnull.push_back(false);
        }
        return true;
    }
    return false;
}

}  // namespace

void ProjectSetState::ExecInit() {
    auto* result_desc = BuildTupleDescFromTargetList(plan->targetlist);
    ps_ResultTupleSlot = TupleTableSlot::Make(result_desc);
    state->es_tupleTable.push_back(ps_ResultTupleSlot);
    ps_ExprContext = CreateExprContext();
    if (leftps != nullptr && leftps->ps_ResultTupleSlot != nullptr) {
        ps_ExprContext->ecxt_scantuple = leftps->ps_ResultTupleSlot;
    }
    ps_queues.resize(plan->targetlist.size());
    ps_done = false;
}

TupleTableSlot* ProjectSetState::ExecProcNode() {
    int natts = static_cast<int>(plan->targetlist.size());

    for (;;) {
        // If we have pending SRF output for the current input tuple, emit it.
        // Find the SRF queue with the most remaining values (the "driving" SRF);
        // we emit while at least one SRF still has values.
        bool any_has_more = false;
        for (int i = 0; i < natts; i++) {
            if (ps_queues[i].is_srf && ps_queues[i].pos < ps_queues[i].values.size()) {
                any_has_more = true;
                break;
            }
        }

        if (any_has_more) {
            ResetExprContext(ps_ExprContext);
            for (int i = 0; i < natts; i++) {
                if (ps_queues[i].is_srf) {
                    if (ps_queues[i].pos < ps_queues[i].values.size()) {
                        ps_ResultTupleSlot->tts_values[i] = ps_queues[i].values[ps_queues[i].pos];
                        ps_ResultTupleSlot->tts_isnull[i] = ps_queues[i].isnull[ps_queues[i].pos];
                        ps_queues[i].pos++;
                    } else {
                        // SRF exhausted: emit NULL (PostgreSQL pads with NULL).
                        ps_ResultTupleSlot->tts_values[i] = 0;
                        ps_ResultTupleSlot->tts_isnull[i] = true;
                    }
                } else {
                    ps_ResultTupleSlot->tts_values[i] = ps_queues[i].scalar_value;
                    ps_ResultTupleSlot->tts_isnull[i] = ps_queues[i].scalar_isnull;
                }
            }
            ps_ResultTupleSlot->tts_nvalid = true;
            ps_ResultTupleSlot->tts_isempty = false;
            return ps_ResultTupleSlot;
        }

        // No pending SRF output: fetch the next input tuple.
        if (ps_done) {
            return nullptr;
        }
        TupleTableSlot* input_slot = nullptr;
        if (leftps != nullptr) {
            input_slot = leftps->ExecProcNode();
        }
        if (input_slot == nullptr) {
            ps_done = true;
            return nullptr;
        }
        ResetExprContext(ps_ExprContext);
        ps_ExprContext->ecxt_scantuple = input_slot;

        // Evaluate each target entry for the new input tuple.
        for (int i = 0; i < natts; i++) {
            Node* expr = plan->targetlist[i]->expr;
            std::vector<Datum> vals;
            std::vector<bool> nulls;
            if (EvalSRFValues(expr, ps_ExprContext, vals, nulls)) {
                ps_queues[i].is_srf = true;
                ps_queues[i].values = std::move(vals);
                ps_queues[i].isnull = std::move(nulls);
                ps_queues[i].pos = 0;
            } else {
                bool isnull = false;
                Datum v = ExecEvalExpr(expr, ps_ExprContext, &isnull);
                ps_queues[i].is_srf = false;
                ps_queues[i].scalar_value = v;
                ps_queues[i].scalar_isnull = isnull;
                ps_queues[i].values.clear();
                ps_queues[i].isnull.clear();
                ps_queues[i].pos = 0;
            }
        }

        // If there are no SRFs at all, emit a single row (like a normal
        // projection) and let the loop fetch the next input on the next call.
        bool has_srf = false;
        for (int i = 0; i < natts; i++) {
            if (ps_queues[i].is_srf) {
                has_srf = true;
                break;
            }
        }
        if (!has_srf) {
            for (int i = 0; i < natts; i++) {
                ps_ResultTupleSlot->tts_values[i] = ps_queues[i].scalar_value;
                ps_ResultTupleSlot->tts_isnull[i] = ps_queues[i].scalar_isnull;
            }
            ps_ResultTupleSlot->tts_nvalid = true;
            ps_ResultTupleSlot->tts_isempty = false;
            return ps_ResultTupleSlot;
        }
        // Else loop back to emit the first SRF row.
    }
}

void ProjectSetState::ExecEnd() {
    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
}

void ProjectSetState::ExecReScan() {
    ps_done = false;
    for (auto& q : ps_queues) {
        q.values.clear();
        q.isnull.clear();
        q.pos = 0;
    }
    if (leftps != nullptr) {
        leftps->ExecReScan();
    }
}

}  // namespace pgcpp::executor

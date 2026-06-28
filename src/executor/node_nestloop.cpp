// node_nestloop.cpp — Nested-loop join implementation.
//
// Converted from PostgreSQL 15's src/backend/executor/nodeNestloop.c.
//
// The nested-loop join iterates over all pairs of (outer, inner) tuples.
// For each outer tuple, it scans the entire inner side. When a pair
// matches the join qual, an output tuple is produced.
//
// This implementation supports INNER and LEFT joins. For LEFT joins,
// unmatched outer tuples produce a NULL-padded output row.
#include "pgcpp/executor/node_nestloop.hpp"

#include <new>

#include "pgcpp/executor/estate.hpp"
#include "pgcpp/executor/exec_expr.hpp"
#include "pgcpp/executor/exec_utils.hpp"
#include "pgcpp/executor/plannodes.hpp"
#include "pgcpp/executor/tupletable.hpp"
#include "pgcpp/parser/parsenodes.hpp"
#include "pgcpp/parser/primnodes.hpp"

namespace pgcpp::executor {

using pgcpp::parser::JoinType;

void NestLoopState::ExecInit() {
    auto* nlplan = static_cast<NestLoop*>(plan);
    nl_jointype = nlplan->jointype;

    // Create the result slot from the target list.
    auto* result_desc = BuildTupleDescFromTargetList(nlplan->targetlist);
    ps_ResultTupleSlot = TupleTableSlot::Make(result_desc);
    state->es_tupleTable.push_back(ps_ResultTupleSlot);

    // Create the expression context.
    ps_ExprContext = CreateExprContext();

    // Create outer and inner tuple slots.
    if (leftps != nullptr && leftps->ps_ResultTupleSlot != nullptr) {
        nl_OuterTupleSlot = TupleTableSlot::Make(leftps->ps_ResultTupleSlot->tts_tupleDescriptor);
        state->es_tupleTable.push_back(nl_OuterTupleSlot);
        ps_ExprContext->ecxt_outertuple = nl_OuterTupleSlot;
    }
    if (rightps != nullptr && rightps->ps_ResultTupleSlot != nullptr) {
        nl_InnerTupleSlot = TupleTableSlot::Make(rightps->ps_ResultTupleSlot->tts_tupleDescriptor);
        state->es_tupleTable.push_back(nl_InnerTupleSlot);
        ps_ExprContext->ecxt_innertuple = nl_InnerTupleSlot;
    }

    nl_NeedNewOuter = true;
    nl_MatchedOuter = false;
}

TupleTableSlot* NestLoopState::ExecProcNode() {
    for (;;) {
        // Get a new outer tuple if needed.
        if (nl_NeedNewOuter) {
            TupleTableSlot* outer = nullptr;
            if (leftps != nullptr) {
                outer = leftps->ExecProcNode();
            }
            if (outer == nullptr) {
                return nullptr;  // no more outer tuples
            }
            // Copy into our outer slot.
            nl_OuterTupleSlot->StoreVirtual(outer->tts_values, outer->tts_isnull);
            nl_NeedNewOuter = false;
            nl_MatchedOuter = false;

            // Rescan the inner side for the new outer tuple.
            if (rightps != nullptr) {
                rightps->ExecReScan();
            }
        }

        // Get the next inner tuple.
        TupleTableSlot* inner = nullptr;
        if (rightps != nullptr) {
            inner = rightps->ExecProcNode();
        }
        if (inner == nullptr) {
            // Inner side exhausted for this outer tuple.
            nl_NeedNewOuter = true;
            // For LEFT JOIN: if no match was found, output a NULL-padded row.
            if (nl_jointype == JoinType::kLeft && !nl_MatchedOuter) {
                // Null out the inner slot.
                for (int i = 0; i < nl_InnerTupleSlot->Natts(); i++) {
                    nl_InnerTupleSlot->tts_values[i] = 0;
                    nl_InnerTupleSlot->tts_isnull[i] = true;
                }
                nl_InnerTupleSlot->tts_nvalid = true;
                nl_InnerTupleSlot->tts_isempty = false;
                ResetExprContext(ps_ExprContext);
                ExecProject(plan->targetlist, ps_ExprContext, ps_ResultTupleSlot);
                return ps_ResultTupleSlot;
            }
            continue;
        }

        // Copy into our inner slot.
        nl_InnerTupleSlot->StoreVirtual(inner->tts_values, inner->tts_isnull);

        // Evaluate the join qual.
        ResetExprContext(ps_ExprContext);
        if (ExecQual(plan->qual, ps_ExprContext)) {
            nl_MatchedOuter = true;
            ExecProject(plan->targetlist, ps_ExprContext, ps_ResultTupleSlot);
            return ps_ResultTupleSlot;
        }
        // Qual didn't match; continue to next inner tuple.
    }
}

void NestLoopState::ExecEnd() {
    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
}

void NestLoopState::ExecReScan() {
    nl_NeedNewOuter = true;
    nl_MatchedOuter = false;
    if (leftps != nullptr) {
        leftps->ExecReScan();
    }
    if (rightps != nullptr) {
        rightps->ExecReScan();
    }
}

}  // namespace pgcpp::executor

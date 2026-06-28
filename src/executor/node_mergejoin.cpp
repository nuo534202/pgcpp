// node_mergejoin.cpp — MergeJoin node implementation (join on sorted inputs).
//
// Both children must be sorted on the merge keys. The join works by
// advancing through matching key groups: for each outer key, all
// matching inner tuples are buffered and replayed for each outer
// tuple with the same key.
//
// Supports INNER and LEFT joins. For LEFT joins, unmatched outer
// tuples produce a NULL-padded output row.
#include "pgcpp/executor/node_mergejoin.hpp"

#include <new>

#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/executor/estate.hpp"
#include "pgcpp/executor/exec_expr.hpp"
#include "pgcpp/executor/exec_utils.hpp"
#include "pgcpp/executor/plannodes.hpp"
#include "pgcpp/executor/tupletable.hpp"
#include "pgcpp/parser/parsenodes.hpp"
#include "pgcpp/parser/primnodes.hpp"

namespace mytoydb::executor {

using mytoydb::catalog::Oid;
using mytoydb::nodes::destroyPallocNode;
using mytoydb::nodes::makePallocNode;
using mytoydb::nodes::NodeTag;
using mytoydb::parser::JoinType;
using mytoydb::parser::Node;
using mytoydb::parser::OpExpr;
using mytoydb::parser::Var;
using mytoydb::types::kInt4Oid;

void MergeJoinState::ExecInit() {
    auto* mjplan = static_cast<MergeJoin*>(plan);
    mj_jointype = mjplan->jointype;

    // Extract outer (left) and inner (right) column indices from mergeclauses.
    // Each clause is an OpExpr like `a.id = b.id` with args[0]=outer Var,
    // args[1]=inner Var.
    for (Node* clause : mjplan->mergeclauses) {
        if (clause != nullptr && clause->GetTag() == NodeTag::kOpExpr) {
            auto* op = static_cast<OpExpr*>(clause);
            if (op->args.size() >= 2 && op->args[0]->GetTag() == NodeTag::kVar &&
                op->args[1]->GetTag() == NodeTag::kVar) {
                auto* outer_var = static_cast<Var*>(op->args[0]);
                auto* inner_var = static_cast<Var*>(op->args[1]);
                mj_clauses.push_back(outer_var->varattno);  // outer col (1-based)
                // Store inner col index in the high bits — we'll use a parallel vector.
                mj_inner_cols.push_back(inner_var->varattno);
            }
        }
    }

    // Create result slot from target list.
    auto* result_desc = BuildTupleDescFromTargetList(mjplan->targetlist);
    ps_ResultTupleSlot = TupleTableSlot::Make(result_desc);
    state->es_tupleTable.push_back(ps_ResultTupleSlot);

    ps_ExprContext = CreateExprContext();

    // Create outer and inner tuple slots (copies of child slots).
    if (leftps != nullptr && leftps->ps_ResultTupleSlot != nullptr) {
        mj_OuterTupleSlot = TupleTableSlot::Make(leftps->ps_ResultTupleSlot->tts_tupleDescriptor);
        state->es_tupleTable.push_back(mj_OuterTupleSlot);
        ps_ExprContext->ecxt_outertuple = mj_OuterTupleSlot;
    }
    if (rightps != nullptr && rightps->ps_ResultTupleSlot != nullptr) {
        mj_InnerTupleSlot = TupleTableSlot::Make(rightps->ps_ResultTupleSlot->tts_tupleDescriptor);
        state->es_tupleTable.push_back(mj_InnerTupleSlot);
        ps_ExprContext->ecxt_innertuple = mj_InnerTupleSlot;
    }

    mj_NeedNewOuter = true;
    mj_NeedNewInner = true;
    mj_MatchedOuter = false;
}

// Compare outer and inner tuples on the merge key columns.
bool MergeJoinState::TuplesMatch(TupleTableSlot* outer, TupleTableSlot* inner) {
    for (size_t i = 0; i < mj_clauses.size() && i < mj_inner_cols.size(); i++) {
        int outer_attno = mj_clauses[i];
        int inner_attno = mj_inner_cols[i];
        if (outer_attno < 1 || inner_attno < 1)
            continue;
        int oidx = outer_attno - 1;
        int iidx = inner_attno - 1;
        if (oidx >= outer->Natts() || iidx >= inner->Natts())
            continue;
        Oid typid = kInt4Oid;
        if (outer->tts_tupleDescriptor != nullptr && oidx < outer->tts_tupleDescriptor->natts)
            typid = outer->tts_tupleDescriptor->attrs[oidx].atttypid;
        int cmp = CompareDatumValues(outer->tts_values[oidx], outer->tts_isnull[oidx],
                                     inner->tts_values[iidx], inner->tts_isnull[iidx], typid);
        if (cmp != 0)
            return false;
    }
    return true;
}

TupleTableSlot* MergeJoinState::ExecProcNode() {
    for (;;) {
        // Get a new outer tuple if needed.
        if (mj_NeedNewOuter) {
            TupleTableSlot* outer = (leftps != nullptr) ? leftps->ExecProcNode() : nullptr;
            if (outer == nullptr)
                return nullptr;
            mj_OuterTupleSlot->StoreVirtual(outer->tts_values, outer->tts_isnull);
            mj_NeedNewOuter = false;
            mj_MatchedOuter = false;
            mj_NeedNewInner = true;
            // Buffer of matching inner tuples for this key group.
            mj_buffer.clear();
            mj_buffer_index = 0;
            // Rescan the inner side so we can find matches for the new outer key.
            // (A true merge join would use mark/restore on the inner to skip
            // already-processed tuples; this implementation rescans from the
            // start for simplicity, at the cost of O(N*M) behavior.)
            if (rightps != nullptr)
                rightps->ExecReScan();
        }

        // If we have buffered inner tuples, replay them.
        if (mj_buffer_index < mj_buffer.size()) {
            TupleTableSlot* buffered_inner = mj_buffer[mj_buffer_index++];
            mj_InnerTupleSlot->StoreVirtual(buffered_inner->tts_values, buffered_inner->tts_isnull);
            ResetExprContext(ps_ExprContext);
            if (ExecQual(plan->qual, ps_ExprContext)) {
                mj_MatchedOuter = true;
                ExecProject(plan->targetlist, ps_ExprContext, ps_ResultTupleSlot);
                return ps_ResultTupleSlot;
            }
            continue;
        }

        // Need to read more inner tuples to find matches.
        if (mj_NeedNewInner) {
            // Read inner tuples until we find one that matches or exceeds the outer key.
            for (;;) {
                TupleTableSlot* inner = (rightps != nullptr) ? rightps->ExecProcNode() : nullptr;
                if (inner == nullptr) {
                    mj_NeedNewInner = false;
                    break;
                }
                // Copy into inner slot.
                mj_InnerTupleSlot->StoreVirtual(inner->tts_values, inner->tts_isnull);

                if (TuplesMatch(mj_OuterTupleSlot, mj_InnerTupleSlot)) {
                    // Found a match — buffer it.
                    TupleTableSlot* copy = TupleTableSlot::Make(inner->tts_tupleDescriptor);
                    copy->StoreVirtual(inner->tts_values, inner->tts_isnull);
                    mj_buffer.push_back(copy);
                    // Try to emit this match.
                    mj_buffer_index = 0;
                    // Fall through to replay below.
                    break;
                }
                // Inner didn't match — check if inner < outer (advance inner) or > outer (stop).
                // For simplicity, just advance inner until match or exhaustion.
            }
            mj_NeedNewInner = false;
            if (mj_buffer.empty()) {
                // No matching inner tuples for this outer.
                if (mj_jointype == JoinType::kLeft && !mj_MatchedOuter) {
                    // Emit NULL-padded inner for LEFT JOIN.
                    for (int i = 0; i < mj_InnerTupleSlot->Natts(); i++) {
                        mj_InnerTupleSlot->tts_values[i] = 0;
                        mj_InnerTupleSlot->tts_isnull[i] = true;
                    }
                    mj_InnerTupleSlot->tts_nvalid = true;
                    mj_InnerTupleSlot->tts_isempty = false;
                    ResetExprContext(ps_ExprContext);
                    ExecProject(plan->targetlist, ps_ExprContext, ps_ResultTupleSlot);
                    mj_NeedNewOuter = true;
                    return ps_ResultTupleSlot;
                }
                mj_NeedNewOuter = true;
                continue;
            }
            // Replay the first buffered match.
            mj_buffer_index = 0;
            TupleTableSlot* buffered_inner = mj_buffer[mj_buffer_index++];
            mj_InnerTupleSlot->StoreVirtual(buffered_inner->tts_values, buffered_inner->tts_isnull);
            ResetExprContext(ps_ExprContext);
            if (ExecQual(plan->qual, ps_ExprContext)) {
                mj_MatchedOuter = true;
                ExecProject(plan->targetlist, ps_ExprContext, ps_ResultTupleSlot);
                return ps_ResultTupleSlot;
            }
            continue;
        }

        // No more inner tuples to read and buffer is exhausted.
        if (mj_jointype == JoinType::kLeft && !mj_MatchedOuter) {
            for (int i = 0; i < mj_InnerTupleSlot->Natts(); i++) {
                mj_InnerTupleSlot->tts_values[i] = 0;
                mj_InnerTupleSlot->tts_isnull[i] = true;
            }
            mj_InnerTupleSlot->tts_nvalid = true;
            mj_InnerTupleSlot->tts_isempty = false;
            ResetExprContext(ps_ExprContext);
            ExecProject(plan->targetlist, ps_ExprContext, ps_ResultTupleSlot);
            mj_NeedNewOuter = true;
            return ps_ResultTupleSlot;
        }
        mj_NeedNewOuter = true;
    }
}

void MergeJoinState::ExecEnd() {
    for (TupleTableSlot* slot : mj_buffer) {
        destroyPallocNode(slot);
    }
    mj_buffer.clear();

    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
}

void MergeJoinState::ExecReScan() {
    for (TupleTableSlot* slot : mj_buffer) {
        destroyPallocNode(slot);
    }
    mj_buffer.clear();
    mj_buffer_index = 0;
    mj_NeedNewOuter = true;
    mj_NeedNewInner = true;
    mj_MatchedOuter = false;
    if (leftps != nullptr)
        leftps->ExecReScan();
    if (rightps != nullptr)
        rightps->ExecReScan();
}

}  // namespace mytoydb::executor

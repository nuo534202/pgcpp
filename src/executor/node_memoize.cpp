// node_memoize.cpp — Memoize node implementation.
//
// Caches the child plan's output keyed by parameter values (evaluated from
// param_exprs in the outer tuple's context). On a key hit, cached rows are
// replayed without re-executing the child; on a miss, the child is drained
// and its output stored under the new key.
//
// Simplification: keys are by-value datums only (no by-ref text keys). This
// suffices for integer-parameterized nested-loop joins.
#include "executor/node_memoize.hpp"

#include "common/containers/node.hpp"
#include "executor/estate.hpp"
#include "executor/exec_expr.hpp"
#include "executor/exec_utils.hpp"
#include "executor/plannodes.hpp"
#include "executor/tupletable.hpp"
#include "types/datum.hpp"

namespace pgcpp::executor {

using pgcpp::nodes::destroyPallocNode;
using pgcpp::parser::Node;
using pgcpp::types::Datum;

void MemoizeState::ExecInit() {
    auto* result_desc = BuildTupleDescFromTargetList(plan->targetlist);
    ps_ResultTupleSlot = TupleTableSlot::Make(result_desc);
    state->es_tupleTable.push_back(ps_ResultTupleSlot);
    ps_ExprContext = CreateExprContext();
    if (leftps != nullptr && leftps->ps_ResultTupleSlot != nullptr) {
        ps_ExprContext->ecxt_scantuple = leftps->ps_ResultTupleSlot;
    }
    ms_first_call = true;
}

TupleTableSlot* MemoizeState::ExecProcNode() {
    auto* mplan = static_cast<Memoize*>(plan);

    for (;;) {
        // If we have an active cache entry with unread rows, emit the next one.
        if (ms_cur != ms_cache.end() && ms_cur->second.read_pos < ms_cur->second.rows.size()) {
            TupleTableSlot* src = ms_cur->second.rows[ms_cur->second.read_pos++];
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

        // Active entry exhausted (or no active entry). If we just finished
        // replaying an entry, return nullptr so the parent advances the outer
        // tuple. On the next call, ms_cur == end() triggers key re-evaluation.
        if (ms_cur != ms_cache.end()) {
            ms_cur = ms_cache.end();
            return nullptr;
        }

        // No active entry: evaluate key from outer context.
        // The outer tuple must be set by the parent (Nested Loop) before
        // calling us. Evaluate the parameter expressions to form the key.
        ResetExprContext(ps_ExprContext);
        std::vector<Datum> key;
        key.reserve(mplan->param_exprs.size());
        for (Node* expr : mplan->param_exprs) {
            bool isnull = false;
            key.push_back(ExecEvalExpr(expr, ps_ExprContext, &isnull));
        }

        auto it = ms_cache.find(key);
        if (it == ms_cache.end()) {
            // Cache miss: drain the child and store its output under this key.
            auto& entry = ms_cache[key];
            if (leftps != nullptr) {
                for (;;) {
                    TupleTableSlot* child_slot = leftps->ExecProcNode();
                    if (child_slot == nullptr) {
                        break;
                    }
                    TupleTableSlot* copy = TupleTableSlot::Make(child_slot->tts_tupleDescriptor);
                    copy->StoreVirtual(child_slot->tts_values, child_slot->tts_isnull);
                    entry.rows.push_back(copy);
                }
                // Rescan the child for the next key lookup.
                leftps->ExecReScan();
            }
            it = ms_cache.find(key);
        }
        ms_cur = it;
        ms_cur->second.read_pos = 0;
        ms_first_call = false;

        // If the entry has no rows (empty result), signal end-of-this-lookup
        // by returning nullptr so the parent moves to the next outer tuple.
        if (ms_cur->second.rows.empty()) {
            ms_cur = ms_cache.end();
            return nullptr;
        }
        // Loop back to emit the first cached row.
    }
}

void MemoizeState::ExecEnd() {
    for (auto& kv : ms_cache) {
        for (TupleTableSlot* slot : kv.second.rows) {
            destroyPallocNode(slot);
        }
    }
    ms_cache.clear();
    ms_cur = ms_cache.end();
    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
}

void MemoizeState::ExecReScan() {
    // Reset read positions so cached entries can be replayed.
    for (auto& kv : ms_cache) {
        kv.second.read_pos = 0;
    }
    ms_cur = ms_cache.end();
    ms_first_call = true;
}

}  // namespace pgcpp::executor

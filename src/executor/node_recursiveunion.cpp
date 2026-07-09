// node_recursiveunion.cpp — RecursiveUnion node (WITH RECURSIVE).
//
// Converted from PostgreSQL 15's src/backend/execProcnode/nodeRecursiveunion.c.
//
// lefttree is the seed term; righttree is the recursive term. Each iteration:
//   1. The working table (registered on EState under wtParam) holds the new
//      rows from the previous iteration.
//   2. The recursive term is re-scanned and drained; its output becomes the
//      next working table and is appended to the cumulative result.
// Iteration stops when the recursive term produces no new rows.
//
// pgcpp simplification: duplicate removal (UNION, not UNION ALL) is not
// implemented — the regenColIdx field is ignored. This matches the existing
// SetOp node's role in PostgreSQL's plan tree for UNION DISTINCT.
#include "executor/node_recursiveunion.hpp"

#include "common/containers/node.hpp"
#include "executor/estate.hpp"
#include "executor/exec_expr.hpp"
#include "executor/exec_utils.hpp"
#include "executor/plannodes.hpp"
#include "executor/tupletable.hpp"

namespace pgcpp::executor {

using pgcpp::nodes::destroyPallocNode;
using pgcpp::nodes::makePallocNode;

namespace {

// Copy a child slot's values into the result slot (passthrough).
void CopyToResult(TupleTableSlot* result, TupleTableSlot* src, ExprContext* econtext) {
    ResetExprContext(econtext);
    int natts = result->Natts();
    int src_natts = src->Natts();
    int ncopy = natts < src_natts ? natts : src_natts;
    for (int i = 0; i < ncopy; i++) {
        result->tts_values[i] = src->tts_values[i];
        result->tts_isnull[i] = src->tts_isnull[i];
    }
    result->tts_nvalid = true;
    result->tts_isempty = false;
}

}  // namespace

void RecursiveUnionState::ExecInit() {
    auto* ruplan = static_cast<RecursiveUnion*>(plan);
    ru_wtParam = ruplan->wtParam;

    // Build the result slot from the target list. RecursiveUnion passes
    // through child columns unchanged (like Append).
    auto* result_desc = BuildTupleDescFromTargetList(plan->targetlist);
    ps_ResultTupleSlot = TupleTableSlot::Make(result_desc);
    state->es_tupleTable.push_back(ps_ResultTupleSlot);
    ps_ExprContext = CreateExprContext();

    // Register the working table on the EState so the WorkTableScan inside
    // the recursive term can locate it by wtParam. The EState destructor
    // frees the WorkTableState.
    auto* wt = makePallocNode<WorkTableState>();
    state->es_worktables[ru_wtParam] = wt;

    ru_initialized = false;
    ru_done = false;
    ru_result_index = 0;
}

TupleTableSlot* RecursiveUnionState::ExecProcNode() {
    if (!ru_initialized) {
        // Seed phase: drain lefttree (seed) into the result and seed the
        // working table with the seed's output.
        std::vector<TupleTableSlot*> seed_tuples;
        if (leftps != nullptr) {
            for (;;) {
                TupleTableSlot* child = leftps->ExecProcNode();
                if (child == nullptr)
                    break;
                TupleTableSlot* copy = TupleTableSlot::Make(child->tts_tupleDescriptor);
                copy->StoreVirtual(child->tts_values, child->tts_isnull);
                ru_results.push_back(copy);
                seed_tuples.push_back(copy);
            }
        }
        ru_initialized = true;

        WorkTableState* wt = state->es_worktables[ru_wtParam];
        wt->tuples = std::move(seed_tuples);
        wt->index = 0;
    }

    // Return buffered results first.
    if (ru_result_index < ru_results.size()) {
        TupleTableSlot* src = ru_results[ru_result_index++];
        CopyToResult(ps_ResultTupleSlot, src, ps_ExprContext);
        return ps_ResultTupleSlot;
    }

    // Results exhausted — run the next recursive iteration if not done.
    if (ru_done)
        return nullptr;

    // Re-scan the recursive term (this resets the WorkTableScan's position)
    // and drain it. The WorkTableScan reads from the working table populated
    // by the previous iteration.
    if (rightps != nullptr)
        rightps->ExecReScan();

    std::vector<TupleTableSlot*> new_tuples;
    if (rightps != nullptr) {
        for (;;) {
            TupleTableSlot* child = rightps->ExecProcNode();
            if (child == nullptr)
                break;
            TupleTableSlot* copy = TupleTableSlot::Make(child->tts_tupleDescriptor);
            copy->StoreVirtual(child->tts_values, child->tts_isnull);
            new_tuples.push_back(copy);
        }
    }

    if (new_tuples.empty()) {
        // Fixpoint reached.
        ru_done = true;
        return nullptr;
    }

    // Append new tuples to the cumulative result and install them as the
    // next working table.
    for (TupleTableSlot* t : new_tuples)
        ru_results.push_back(t);
    WorkTableState* wt = state->es_worktables[ru_wtParam];
    wt->tuples = std::move(new_tuples);
    wt->index = 0;

    // Return the first newly-produced tuple via the buffered-results path.
    return ExecProcNode();
}

void RecursiveUnionState::ExecEnd() {
    // Free cached result slots (owned by us, not by es_tupleTable).
    for (TupleTableSlot* slot : ru_results) {
        destroyPallocNode(slot);
    }
    ru_results.clear();

    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
}

void RecursiveUnionState::ExecReScan() {
    ru_result_index = 0;
    ru_done = false;
    ru_initialized = false;
    for (TupleTableSlot* slot : ru_results) {
        destroyPallocNode(slot);
    }
    ru_results.clear();
    // Re-seed on next ExecProcNode call.
    if (leftps != nullptr)
        leftps->ExecReScan();
}

}  // namespace pgcpp::executor

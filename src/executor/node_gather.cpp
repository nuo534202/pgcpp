// node_gather.cpp — Gather / GatherMerge node implementation.
//
// Converted from PostgreSQL 15's src/backend/execProcnode/nodeGather.c and
// nodeGatherMerge.c.
//
// In PostgreSQL these nodes launch parallel workers and gather their output.
// pgcpp forbids std::thread and uses a fork-based multi-process model that is
// not wired into the executor, so the leader executes the child plan directly
// (the nworkers = 0 serial fallback). GatherMerge additionally sorts the
// child's output on the merge keys via TupleSort.
#include "executor/node_gather.hpp"

#include <new>
#include <vector>

#include "catalog/catalog.hpp"
#include "common/containers/node.hpp"
#include "executor/estate.hpp"
#include "executor/exec_expr.hpp"
#include "executor/exec_utils.hpp"
#include "executor/plannodes.hpp"
#include "executor/tupletable.hpp"
#include "utils/sort/tuplesort.hpp"

namespace pgcpp::executor {

using pgcpp::sort::SortKey;
using pgcpp::sort::TupleSort;
using pgcpp::types::kInt4Oid;

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

// Build sort keys from a GatherMerge plan and the child's tuple descriptor.
std::vector<SortKey> BuildSortKeys(GatherMerge* gmplan, pgcpp::access::TupleDesc child_tupdesc) {
    std::vector<SortKey> sort_keys;
    for (size_t i = 0; i < gmplan->sortColIdx.size(); i++) {
        int idx = gmplan->sortColIdx[i];
        SortKey key;
        key.attnum = idx;
        if (child_tupdesc != nullptr && idx >= 1 && idx <= child_tupdesc->natts) {
            key.typid = child_tupdesc->attrs[idx - 1].atttypid;
        } else {
            key.typid = kInt4Oid;
        }
        key.reverse = gmplan->reverse[i];
        key.nulls_first = gmplan->nullsFirst[i];
        sort_keys.push_back(key);
    }
    return sort_keys;
}

}  // namespace

// --- Gather (serial passthrough) ---

void GatherState::ExecInit() {
    auto* result_desc = BuildTupleDescFromTargetList(plan->targetlist);
    ps_ResultTupleSlot = TupleTableSlot::Make(result_desc);
    state->es_tupleTable.push_back(ps_ResultTupleSlot);
    ps_ExprContext = CreateExprContext();
}

TupleTableSlot* GatherState::ExecProcNode() {
    if (leftps == nullptr)
        return nullptr;
    TupleTableSlot* child = leftps->ExecProcNode();
    if (child == nullptr)
        return nullptr;
    CopyToResult(ps_ResultTupleSlot, child, ps_ExprContext);
    return ps_ResultTupleSlot;
}

void GatherState::ExecEnd() {
    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
}

void GatherState::ExecReScan() {
    if (leftps != nullptr)
        leftps->ExecReScan();
}

// --- GatherMerge (serial passthrough + sort) ---

void GatherMergeState::ExecInit() {
    auto* gmplan = static_cast<GatherMerge*>(plan);
    auto* result_desc = BuildTupleDescFromTargetList(plan->targetlist);
    ps_ResultTupleSlot = TupleTableSlot::Make(result_desc);
    state->es_tupleTable.push_back(ps_ResultTupleSlot);
    ps_ExprContext = CreateExprContext();

    pgcpp::access::TupleDesc child_tupdesc = nullptr;
    if (leftps != nullptr && leftps->ps_ResultTupleSlot != nullptr)
        child_tupdesc = leftps->ps_ResultTupleSlot->tts_tupleDescriptor;
    auto sort_keys = BuildSortKeys(gmplan, child_tupdesc);
    // work_mem defaults to 4MB (PostgreSQL default).
    tuplesort = std::make_unique<TupleSort>(result_desc, std::move(sort_keys),
                                            /*work_mem=*/4 * 1024 * 1024);
}

TupleTableSlot* GatherMergeState::ExecProcNode() {
    if (!sorted_done) {
        // Phase 1: drain the child into TupleSort.
        if (leftps != nullptr) {
            for (;;) {
                TupleTableSlot* child = leftps->ExecProcNode();
                if (child == nullptr)
                    break;
                tuplesort->PutTuple(child);
            }
        }
        tuplesort->PerformSort();
        sorted_done = true;
    }

    TupleTableSlot* src = tuplesort->GetTuple();
    if (src == nullptr)
        return nullptr;
    CopyToResult(ps_ResultTupleSlot, src, ps_ExprContext);
    return ps_ResultTupleSlot;
}

void GatherMergeState::ExecEnd() {
    tuplesort.reset();
    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
}

void GatherMergeState::ExecReScan() {
    tuplesort.reset();
    sorted_done = false;
    if (leftps != nullptr)
        leftps->ExecReScan();
    // Re-initialize the TupleSort for the next scan.
    auto* gmplan = static_cast<GatherMerge*>(plan);
    auto* result_desc = ps_ResultTupleSlot->tts_tupleDescriptor;
    pgcpp::access::TupleDesc child_tupdesc = nullptr;
    if (leftps != nullptr && leftps->ps_ResultTupleSlot != nullptr)
        child_tupdesc = leftps->ps_ResultTupleSlot->tts_tupleDescriptor;
    auto sort_keys = BuildSortKeys(gmplan, child_tupdesc);
    tuplesort = std::make_unique<TupleSort>(result_desc, std::move(sort_keys),
                                            /*work_mem=*/4 * 1024 * 1024);
}

}  // namespace pgcpp::executor

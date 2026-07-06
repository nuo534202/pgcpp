// node_sort.cpp — Sort node implementation using TupleSort (P1-3).
//
// Converted from PostgreSQL 15's src/backend/executor/nodeSort.c.
//
// The Sort node collects all tuples from its child, sorts them by the
// specified sort keys, and returns them one at a time. As of P1-3,
// sorting is delegated to TupleSort, which performs an in-memory sort
// when the data fits in work_mem and an external tape-based merge sort
// when it does not. OFFSET and LIMIT are applied during the output phase.
#include "executor/node_sort.hpp"

#include <new>

#include "catalog/catalog.hpp"
#include "catalog/pg_operator.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "executor/estate.hpp"
#include "executor/exec_expr.hpp"
#include "executor/exec_utils.hpp"
#include "executor/plannodes.hpp"
#include "executor/tupletable.hpp"
#include "types/datum.hpp"
#include "utils/sort/tuplesort.hpp"

namespace pgcpp::executor {

using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::Oid;
using pgcpp::memory::palloc;
using pgcpp::nodes::destroyPallocNode;
using pgcpp::sort::SortKey;
using pgcpp::sort::TupleSort;
using pgcpp::types::Datum;
using pgcpp::types::kInt4Oid;

void SortState::ExecInit() {
    auto* sortplan = static_cast<Sort*>(plan);

    // Create the result slot from the target list.
    auto* result_desc = BuildTupleDescFromTargetList(sortplan->targetlist);
    ps_ResultTupleSlot = TupleTableSlot::Make(result_desc);
    state->es_tupleTable.push_back(ps_ResultTupleSlot);

    // Create the expression context.
    ps_ExprContext = CreateExprContext();
    if (leftps != nullptr && leftps->ps_ResultTupleSlot != nullptr) {
        ps_ExprContext->ecxt_scantuple = leftps->ps_ResultTupleSlot;
    }

    // Build the SortKey list for TupleSort from the plan's sortColIdx and
    // the child's output descriptor (to resolve type OIDs).
    std::vector<SortKey> sort_keys;
    if (leftps != nullptr && leftps->ps_ResultTupleSlot != nullptr &&
        leftps->ps_ResultTupleSlot->tts_tupleDescriptor != nullptr) {
        auto* tupdesc = leftps->ps_ResultTupleSlot->tts_tupleDescriptor;
        for (size_t i = 0; i < sortplan->sortColIdx.size(); i++) {
            int idx = sortplan->sortColIdx[i];
            SortKey key;
            key.attnum = idx;
            if (idx >= 1 && idx <= tupdesc->natts) {
                key.typid = tupdesc->attrs[idx - 1].atttypid;
            } else {
                key.typid = kInt4Oid;
            }
            key.reverse = sortplan->reverse[i];
            key.nulls_first = sortplan->nullsFirst[i];
            sort_keys.push_back(key);
        }
    } else {
        for (size_t i = 0; i < sortplan->sortColIdx.size(); i++) {
            SortKey key;
            key.attnum = sortplan->sortColIdx[i];
            key.typid = kInt4Oid;
            key.reverse = sortplan->reverse[i];
            key.nulls_first = sortplan->nullsFirst[i];
            sort_keys.push_back(key);
        }
    }

    // Construct the TupleSort. work_mem defaults to 4MB (PostgreSQL default).
    // TODO: read from GUCs once work_mem GUC is wired in.
    tuplesort = std::make_unique<TupleSort>(result_desc, std::move(sort_keys),
                                            /*work_mem=*/4 * 1024 * 1024);
}

TupleTableSlot* SortState::ExecProcNode() {
    auto* sortplan = static_cast<Sort*>(plan);

    if (!sorted_done) {
        // Phase 1: collect all tuples from the child into TupleSort.
        for (;;) {
            TupleTableSlot* child_slot = leftps->ExecProcNode();
            if (child_slot == nullptr)
                break;
            tuplesort->PutTuple(child_slot);
        }

        // Finalize the sort (in-memory or external merge).
        tuplesort->PerformSort();
        sorted_done = true;

        // Apply OFFSET: skip the first `offset` sorted tuples.
        int64_t off = sortplan->offset;
        for (int64_t i = 0; i < off; i++) {
            if (tuplesort->GetTuple() == nullptr)
                break;
            rows_skipped++;
        }
    }

    // Apply LIMIT: stop once we've output `limit` tuples.
    if (sortplan->limit >= 0 && rows_output >= sortplan->limit) {
        return nullptr;
    }

    TupleTableSlot* src = tuplesort->GetTuple();
    if (src == nullptr) {
        return nullptr;
    }
    rows_output++;

    // Copy the sorted tuple's values directly into the result slot. The Sort
    // node passes through the child's columns unchanged — it must NOT
    // re-evaluate the target list (which may contain Aggref nodes that
    // require an aggregates slot the Sort does not have).
    ps_ExprContext->ecxt_scantuple = src;
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

void SortState::ExecEnd() {
    // TupleSort (unique_ptr) is destroyed automatically, freeing all
    // resources including temp files.
    tuplesort.reset();

    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
}

void SortState::ExecReScan() {
    tuplesort.reset();
    sorted_done = false;
    rows_skipped = 0;
    rows_output = 0;
    if (leftps != nullptr) {
        leftps->ExecReScan();
    }
    // Re-initialize the TupleSort for the next scan. We need to rebuild
    // the sort keys (in case the child's descriptor changed).
    auto* sortplan = static_cast<Sort*>(plan);
    auto* result_desc = ps_ResultTupleSlot->tts_tupleDescriptor;
    std::vector<SortKey> sort_keys;
    if (leftps != nullptr && leftps->ps_ResultTupleSlot != nullptr &&
        leftps->ps_ResultTupleSlot->tts_tupleDescriptor != nullptr) {
        auto* tupdesc = leftps->ps_ResultTupleSlot->tts_tupleDescriptor;
        for (size_t i = 0; i < sortplan->sortColIdx.size(); i++) {
            int idx = sortplan->sortColIdx[i];
            SortKey key;
            key.attnum = idx;
            if (idx >= 1 && idx <= tupdesc->natts) {
                key.typid = tupdesc->attrs[idx - 1].atttypid;
            } else {
                key.typid = kInt4Oid;
            }
            key.reverse = sortplan->reverse[i];
            key.nulls_first = sortplan->nullsFirst[i];
            sort_keys.push_back(key);
        }
    } else {
        for (size_t i = 0; i < sortplan->sortColIdx.size(); i++) {
            SortKey key;
            key.attnum = sortplan->sortColIdx[i];
            key.typid = kInt4Oid;
            key.reverse = sortplan->reverse[i];
            key.nulls_first = sortplan->nullsFirst[i];
            sort_keys.push_back(key);
        }
    }
    tuplesort = std::make_unique<TupleSort>(result_desc, std::move(sort_keys),
                                            /*work_mem=*/4 * 1024 * 1024);
}

}  // namespace pgcpp::executor

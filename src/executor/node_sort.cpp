// node_sort.cpp — Sort node implementation with Top-N optimization.
//
// Converted from PostgreSQL 15's src/backend/executor/nodeSort.c.
//
// The Sort node collects all tuples from its child, sorts them by the
// specified sort keys, and returns them one at a time. When a limit is
// specified, only the top N tuples are kept (Top-N heapsort optimization).
#include "mytoydb/executor/node_sort.h"

#include <algorithm>
#include <new>

#include "mytoydb/catalog/catalog.h"
#include "mytoydb/catalog/pg_operator.h"
#include "mytoydb/common/error/elog.h"
#include "mytoydb/executor/estate.h"
#include "mytoydb/executor/exec_expr.h"
#include "mytoydb/executor/exec_utils.h"
#include "mytoydb/executor/plannodes.h"
#include "mytoydb/executor/tupletable.h"
#include "mytoydb/types/datum.h"

namespace mytoydb::executor {

using mytoydb::catalog::GetCatalog;
using mytoydb::catalog::Oid;
using mytoydb::memory::palloc;
using mytoydb::types::Datum;
using mytoydb::types::DatumGetBool;
using mytoydb::types::DatumGetFloat8;
using mytoydb::types::DatumGetInt32;
using mytoydb::types::DatumGetInt64;
using mytoydb::types::DatumGetTextP;
using mytoydb::types::kBoolOid;
using mytoydb::types::kFloat8Oid;
using mytoydb::types::kInt4Oid;
using mytoydb::types::kInt8Oid;
using mytoydb::types::kTextOid;
using mytoydb::types::VARDATA;
using mytoydb::types::VARSIZE_DATA;

namespace {

// Compare two Datum values of the given type.
// Returns -1 if a < b, 0 if a == b, 1 if a > b.
int CompareValues(Datum a, Datum b, Oid typid) {
    switch (typid) {
        case kInt4Oid: {
            int32_t va = DatumGetInt32(a);
            int32_t vb = DatumGetInt32(b);
            if (va < vb)
                return -1;
            if (va > vb)
                return 1;
            return 0;
        }
        case kInt8Oid: {
            int64_t va = DatumGetInt64(a);
            int64_t vb = DatumGetInt64(b);
            if (va < vb)
                return -1;
            if (va > vb)
                return 1;
            return 0;
        }
        case kFloat8Oid: {
            double va = DatumGetFloat8(a);
            double vb = DatumGetFloat8(b);
            if (va < vb)
                return -1;
            if (va > vb)
                return 1;
            return 0;
        }
        case kBoolOid: {
            bool va = DatumGetBool(a);
            bool vb = DatumGetBool(b);
            if (va == vb)
                return 0;
            return va ? 1 : -1;
        }
        case kTextOid: {
            const char* pa = DatumGetTextP(a);
            const char* pb = DatumGetTextP(b);
            int la = VARSIZE_DATA(pa);
            int lb = VARSIZE_DATA(pb);
            int min_len = la < lb ? la : lb;
            int cmp = std::memcmp(VARDATA(pa), VARDATA(pb), min_len);
            if (cmp != 0)
                return cmp < 0 ? -1 : 1;
            if (la < lb)
                return -1;
            if (la > lb)
                return 1;
            return 0;
        }
        default:
            return 0;
    }
}

}  // namespace

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
}

void SortState::SortTuples() {
    auto* sortplan = static_cast<Sort*>(plan);

    // Determine the type of each sort column from the child's output descriptor.
    std::vector<Oid> sort_types;
    if (leftps != nullptr && leftps->ps_ResultTupleSlot != nullptr &&
        leftps->ps_ResultTupleSlot->tts_tupleDescriptor != nullptr) {
        auto* tupdesc = leftps->ps_ResultTupleSlot->tts_tupleDescriptor;
        for (int idx : sortplan->sortColIdx) {
            if (idx >= 1 && idx <= tupdesc->natts) {
                sort_types.push_back(tupdesc->attrs[idx - 1].atttypid);
            } else {
                sort_types.push_back(kInt4Oid);
            }
        }
    } else {
        for (size_t i = 0; i < sortplan->sortColIdx.size(); i++) {
            sort_types.push_back(kInt4Oid);
        }
    }

    std::sort(
        sorted_tuples.begin(), sorted_tuples.end(), [&](TupleTableSlot* a, TupleTableSlot* b) {
            for (size_t i = 0; i < sortplan->sortColIdx.size(); i++) {
                int attno = sortplan->sortColIdx[i];
                bool a_null = (attno >= 1 && attno <= a->Natts()) ? a->tts_isnull[attno - 1] : true;
                bool b_null = (attno >= 1 && attno <= b->Natts()) ? b->tts_isnull[attno - 1] : true;

                // Handle NULLs.
                if (a_null && b_null)
                    continue;
                if (a_null) {
                    return static_cast<bool>(
                        sortplan->nullsFirst[i]);  // a comes first if nullsFirst
                }
                if (b_null) {
                    return !static_cast<bool>(
                        sortplan->nullsFirst[i]);  // b comes first if nullsFirst
                }

                int cmp = CompareValues(a->tts_values[attno - 1], b->tts_values[attno - 1],
                                        sort_types[i]);

                if (cmp == 0)
                    continue;
                if (sortplan->reverse[i])
                    cmp = -cmp;
                return cmp < 0;
            }
            return false;  // equal
        });
}

TupleTableSlot* SortState::ExecProcNode() {
    if (!sorted_done) {
        // Phase 1: collect all tuples from the child.
        for (;;) {
            TupleTableSlot* child_slot = leftps->ExecProcNode();
            if (child_slot == nullptr)
                break;

            // Copy the child slot into a new slot we own.
            TupleTableSlot* copy = TupleTableSlot::Make(child_slot->tts_tupleDescriptor);
            copy->StoreVirtual(child_slot->tts_values, child_slot->tts_isnull);
            sorted_tuples.push_back(copy);
        }

        // Sort the tuples.
        SortTuples();
        sorted_done = true;
        output_index = 0;

        // Apply Top-N limit.
        auto* sortplan = static_cast<Sort*>(plan);
        if (sortplan->limit >= 0 && static_cast<int64_t>(sorted_tuples.size()) > sortplan->limit) {
            sorted_tuples.resize(sortplan->limit);
        }
    }

    // Phase 2: output sorted tuples.
    if (output_index >= sorted_tuples.size()) {
        return nullptr;
    }
    TupleTableSlot* src = sorted_tuples[output_index++];

    // Project into the result slot.
    ps_ExprContext->ecxt_scantuple = src;
    ResetExprContext(ps_ExprContext);
    ExecProject(plan->targetlist, ps_ExprContext, ps_ResultTupleSlot);
    return ps_ResultTupleSlot;
}

void SortState::ExecEnd() {
    // Free copied slots.
    for (TupleTableSlot* slot : sorted_tuples) {
        if (slot != nullptr) {
            slot->~TupleTableSlot();
            mytoydb::memory::pfree(slot);
        }
    }
    sorted_tuples.clear();

    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
}

void SortState::ExecReScan() {
    for (TupleTableSlot* slot : sorted_tuples) {
        if (slot != nullptr) {
            slot->~TupleTableSlot();
            mytoydb::memory::pfree(slot);
        }
    }
    sorted_tuples.clear();
    sorted_done = false;
    output_index = 0;
    if (leftps != nullptr) {
        leftps->ExecReScan();
    }
}

}  // namespace mytoydb::executor

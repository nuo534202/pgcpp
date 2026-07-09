// node_bitmap_heapscan.cpp — BitmapHeapScan node implementation.
//
// Reads the TID bitmap produced by the BitmapIndexScan child (lefttree),
// then for each TID fetches the corresponding heap tuple, applies the
// residual qual, and projects the target list.
//
// The heap fetch uses a sequential heap scan to locate the tuple by TID,
// mirroring the simplified approach in node_indexscan.cpp. A production
// implementation would use heap_fetch() for direct page-level access.
#include "executor/node_bitmap_heapscan.hpp"

#include "access/heapam.hpp"
#include "access/rel.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "executor/estate.hpp"
#include "executor/exec_expr.hpp"
#include "executor/exec_utils.hpp"
#include "executor/node_bitmap_indexscan.hpp"
#include "executor/plannodes.hpp"
#include "executor/tupletable.hpp"
#include "parser/parsenodes.hpp"
#include "transaction/heap_tuple.hpp"

namespace pgcpp::executor {

using pgcpp::access::heap_beginscan;
using pgcpp::access::heap_endscan;
using pgcpp::access::heap_getnext;
using pgcpp::access::HeapScanDesc;
using pgcpp::access::RelationOpen;
using pgcpp::parser::RangeTblEntry;
using pgcpp::transaction::HeapTuple;
using pgcpp::transaction::ItemPointerData;

void BitmapHeapScanState::ExecInit() {
    auto* heapplan = static_cast<BitmapHeapScan*>(plan);

    int rtindex = heapplan->scanrelid - 1;
    if (rtindex < 0 || rtindex >= static_cast<int>(state->es_range_table.size())) {
        ereport(pgcpp::error::LogLevel::kError, "BitmapHeapScan: invalid scanrelid");
    }
    RangeTblEntry* rte = state->es_range_table[rtindex];

    bhss_relation = RelationOpen(static_cast<pgcpp::catalog::Oid>(rte->relid));
    if (bhss_relation == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "BitmapHeapScan: relation not found");
    }
    state->es_open_relations.push_back(bhss_relation);

    bhss_ScanTupleSlot = TupleTableSlot::Make(bhss_relation->rd_att);
    state->es_tupleTable.push_back(bhss_ScanTupleSlot);

    auto* result_desc = BuildTupleDescFromTargetList(heapplan->targetlist);
    ps_ResultTupleSlot = TupleTableSlot::Make(result_desc);
    state->es_tupleTable.push_back(ps_ResultTupleSlot);

    ps_ExprContext = CreateExprContext();
    ps_ExprContext->ecxt_scantuple = bhss_ScanTupleSlot;

    bitmap_fetched = false;
    tid_index = 0;
    tids.clear();
}

TupleTableSlot* BitmapHeapScanState::ExecProcNode() {
    // On the first call, drive the BitmapIndexScan child once to populate
    // its TID vector, then copy the vector locally.
    if (!bitmap_fetched) {
        if (leftps != nullptr) {
            leftps->ExecProcNode();  // builds the bitmap (returns nullptr)
            auto* child = dynamic_cast<BitmapIndexScanState*>(leftps);
            if (child != nullptr) {
                tids = child->tids;
            }
        }
        bitmap_fetched = true;
        tid_index = 0;
    }

    for (;;) {
        if (tid_index >= tids.size()) {
            return nullptr;  // all TIDs consumed
        }

        ItemPointerData target_tid = tids[tid_index++];

        // Fetch the heap tuple at target_tid via a sequential scan (same
        // simplified approach as node_indexscan.cpp).
        HeapScanDesc hscan = heap_beginscan(bhss_relation, state->es_snapshot);
        HeapTuple tuple = nullptr;
        while ((tuple = heap_getnext(hscan)) != nullptr) {
            if (tuple->t_self == target_tid)
                break;
        }
        heap_endscan(hscan);

        if (tuple == nullptr) {
            continue;  // TID not found (tuple may have been deleted)
        }

        bhss_ScanTupleSlot->StoreTuple(tuple, false);

        // Apply the residual qual.
        ResetExprContext(ps_ExprContext);
        if (!ExecQual(plan->qual, ps_ExprContext)) {
            continue;
        }

        ExecProject(plan->targetlist, ps_ExprContext, ps_ResultTupleSlot);
        return ps_ResultTupleSlot;
    }
}

void BitmapHeapScanState::ExecEnd() {
    bhss_relation = nullptr;
    tids.clear();
    bitmap_fetched = false;
    tid_index = 0;

    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
}

void BitmapHeapScanState::ExecReScan() {
    tids.clear();
    tid_index = 0;
    bitmap_fetched = false;
    if (leftps != nullptr)
        leftps->ExecReScan();
}

}  // namespace pgcpp::executor

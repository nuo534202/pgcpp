// node_tidscan.cpp — TidScan node implementation (scan by specific TIDs).
//
// Fetches each tuple at the listed TIDs directly via heap_fetch_by_tid,
// stores it in the scan slot, applies the qual filter, and projects the
// target list.
#include "executor/node_tidscan.hpp"

#include "access/heapam.hpp"
#include "access/rel.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "executor/estate.hpp"
#include "executor/exec_expr.hpp"
#include "executor/exec_utils.hpp"
#include "executor/plannodes.hpp"
#include "executor/tupletable.hpp"
#include "parser/parsenodes.hpp"
#include "transaction/heap_tuple.hpp"

namespace pgcpp::executor {

using pgcpp::access::heap_fetch_by_tid;
using pgcpp::access::heap_freetuple;
using pgcpp::access::RelationOpen;
using pgcpp::parser::RangeTblEntry;

void TidScanState::ExecInit() {
    auto* tidplan = static_cast<TidScan*>(plan);

    int rtindex = tidplan->scanrelid - 1;  // 1-based to 0-based
    if (rtindex < 0 || rtindex >= static_cast<int>(state->es_range_table.size())) {
        ereport(pgcpp::error::LogLevel::kError, "TidScan: invalid scanrelid");
    }
    RangeTblEntry* rte = state->es_range_table[rtindex];

    ts_relation = RelationOpen(static_cast<pgcpp::catalog::Oid>(rte->relid));
    if (ts_relation == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "TidScan: relation not found");
    }
    state->es_open_relations.push_back(ts_relation);

    ts_ScanTupleSlot = TupleTableSlot::Make(ts_relation->rd_att);
    state->es_tupleTable.push_back(ts_ScanTupleSlot);

    auto* result_desc = BuildTupleDescFromTargetList(tidplan->targetlist);
    ps_ResultTupleSlot = TupleTableSlot::Make(result_desc);
    state->es_tupleTable.push_back(ps_ResultTupleSlot);

    ps_ExprContext = CreateExprContext();
    ps_ExprContext->ecxt_scantuple = ts_ScanTupleSlot;
    ts_tid_index = 0;
}

TupleTableSlot* TidScanState::ExecProcNode() {
    auto* tidplan = static_cast<TidScan*>(plan);

    for (; ts_tid_index < tidplan->tids.size(); ts_tid_index++) {
        pgcpp::transaction::ItemPointerData tid = tidplan->tids[ts_tid_index];
        pgcpp::transaction::HeapTuple tup = heap_fetch_by_tid(ts_relation, tid);
        if (tup == nullptr) {
            continue;  // dead/unused TID
        }
        ts_ScanTupleSlot->StoreTuple(tup, /*shouldFree=*/true);
        ResetExprContext(ps_ExprContext);
        ps_ExprContext->ecxt_scantuple = ts_ScanTupleSlot;

        // Advance past this TID before returning (so the next call continues).
        ts_tid_index++;

        if (plan->qual != nullptr && !ExecQual(plan->qual, ps_ExprContext)) {
            continue;
        }
        ExecProject(plan->targetlist, ps_ExprContext, ps_ResultTupleSlot);
        return ps_ResultTupleSlot;
    }
    return nullptr;
}

void TidScanState::ExecEnd() {
    ts_relation = nullptr;
    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
}

void TidScanState::ExecReScan() {
    ts_tid_index = 0;
}

}  // namespace pgcpp::executor

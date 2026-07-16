// node_seqscan.cpp — Sequential scan node implementation.
//
// Converted from PostgreSQL 15's src/backend/executor/nodeSeqscan.c.
//
// SeqScan reads tuples from a heap relation in physical order, applies
// the qual filter (WHERE clause), and projects the target list to
// produce output tuples.
//
// P3-9: When the scanned relation is a pg_stat_* view (detected via
// IsStatsView), the scan uses the virtual StatsScanDesc instead of a
// HeapScanDesc. This allows SELECT * FROM pg_stat_database etc. to work
// without a physical heap file.
#include "executor/node_seqscan.hpp"

#include <new>

#include "access/heapam.hpp"
#include "access/rel.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/memory_context.hpp"
#include "executor/estate.hpp"
#include "executor/exec_expr.hpp"
#include "executor/exec_utils.hpp"
#include "executor/plannodes.hpp"
#include "executor/tupletable.hpp"
#include "parser/parsenodes.hpp"
#include "stats/stats_collector.hpp"
#include "stats/stats_view.hpp"
#include "transaction/heap_tuple.hpp"

namespace pgcpp::executor {

using pgcpp::access::heap_beginscan;
using pgcpp::access::heap_endscan;
using pgcpp::access::heap_getnext;
using pgcpp::access::heap_rescan;
using pgcpp::access::Relation;
using pgcpp::access::RelationClose;
using pgcpp::access::RelationOpen;
using pgcpp::access::TupleDesc;
using pgcpp::memory::palloc;
using pgcpp::parser::RangeTblEntry;
using pgcpp::stats::GetStatsCollector;
using pgcpp::stats::IsStatsView;
using pgcpp::stats::StatsBeginScan;
using pgcpp::stats::StatsEndScan;
using pgcpp::stats::StatsGetNext;
using pgcpp::stats::StatsScanDesc;
using pgcpp::transaction::HeapTuple;

void SeqScanState::ExecInit() {
    auto* seqplan = static_cast<SeqScan*>(this->plan);

    // Look up the range table entry.
    int rtindex = seqplan->scanrelid - 1;  // 1-based to 0-based
    if (rtindex < 0 || rtindex >= static_cast<int>(state->es_range_table.size())) {
        ereport(pgcpp::error::LogLevel::kError, "SeqScan: invalid scanrelid");
    }
    RangeTblEntry* rte = state->es_range_table[rtindex];

    // Open the relation.
    ss_relation = RelationOpen(static_cast<pgcpp::catalog::Oid>(rte->relid));
    if (ss_relation == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "SeqScan: relation not found");
    }
    state->es_open_relations.push_back(ss_relation);

    // Create the scan tuple slot using the relation's tuple descriptor.
    ss_ScanTupleSlot = TupleTableSlot::Make(ss_relation->rd_att);
    state->es_tupleTable.push_back(ss_ScanTupleSlot);

    // P3-9: Check if this is a pg_stat_* view. If so, use the virtual scan
    // instead of a heap scan.
    if (IsStatsView(static_cast<pgcpp::catalog::Oid>(rte->relid))) {
        ss_stats_scan = StatsBeginScan(static_cast<pgcpp::catalog::Oid>(rte->relid));
    } else {
        // Start the heap scan.
        ss_scanDesc = heap_beginscan(ss_relation, state->es_snapshot);
    }

    // Create the result tuple slot from the target list.
    TupleDesc result_desc = BuildTupleDescFromTargetList(seqplan->targetlist);
    ps_ResultTupleSlot = TupleTableSlot::Make(result_desc);
    state->es_tupleTable.push_back(ps_ResultTupleSlot);

    // Create the expression context.
    ps_ExprContext = CreateExprContext();
    ps_ExprContext->ecxt_scantuple = ss_ScanTupleSlot;
}

TupleTableSlot* SeqScanState::ExecProcNode() {
    for (;;) {
        HeapTuple tuple = nullptr;

        if (ss_stats_scan != nullptr) {
            // P3-9: virtual scan for pg_stat_* views.
            tuple = StatsGetNext(ss_stats_scan);
        } else {
            // Fetch the next tuple from the heap scan.
            tuple = heap_getnext(ss_scanDesc);
        }

        if (tuple == nullptr) {
            // P3-9: report seq scan stats for real heap relations.
            if (ss_stats_scan == nullptr && ss_relation != nullptr &&
                ss_relation->rd_id != pgcpp::catalog::kInvalidOid) {
                GetStatsCollector().ReportSeqScan(ss_relation->rd_id, ss_tuples_returned);
            }
            return nullptr;  // scan exhausted
        }

        // Store the tuple in the scan slot (deforms it).
        ss_ScanTupleSlot->StoreTuple(tuple, false);
        ++ss_tuples_returned;

        // Reset the per-tuple memory context.
        ResetExprContext(ps_ExprContext);

        // Evaluate the qual (WHERE clause).
        if (!ExecQual(plan->qual, ps_ExprContext)) {
            continue;  // tuple doesn't pass the filter
        }

        // Project the target list into the result slot.
        ExecProject(plan->targetlist, ps_ExprContext, ps_ResultTupleSlot);
        return ps_ResultTupleSlot;
    }
}

void SeqScanState::ExecEnd() {
    if (ss_stats_scan != nullptr) {
        StatsEndScan(ss_stats_scan);
        ss_stats_scan = nullptr;
    }
    if (ss_scanDesc != nullptr) {
        heap_endscan(ss_scanDesc);
        ss_scanDesc = nullptr;
    }
    // The relation is closed by EState's cleanup (es_open_relations).
    ss_relation = nullptr;

    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
}

void SeqScanState::ExecReScan() {
    if (ss_scanDesc != nullptr) {
        heap_rescan(ss_scanDesc);
    }
    if (ss_stats_scan != nullptr) {
        // Restart the virtual scan from the beginning.
        ss_stats_scan->next_index = 0;
    }
}

}  // namespace pgcpp::executor

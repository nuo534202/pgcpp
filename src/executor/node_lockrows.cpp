// node_lockrows.cpp — LockRows executor node implementation.
//
// Converted from PostgreSQL 15's src/backend/executor/nodeLockRows.c.
//
// For each tuple produced by the child plan, acquires a row-level lock
// by calling heap_lock_tuple. The tuple is then passed through to the
// parent unchanged.
//
// TID access follows the same pattern as ModifyTable: the child's result
// slot may carry the backing HeapTuple in tts_tuple; if projection has
// cleared it, fall back to the child's scan tuple slot (ecxt_scantuple),
// which holds the original heap tuple read from the table.
#include "executor/node_lockrows.hpp"

#include "access/heapam.hpp"
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

using pgcpp::access::heap_lock_tuple;
using pgcpp::access::RelationOpen;
using pgcpp::nodes::makePallocNode;
using pgcpp::parser::RangeTblEntry;
using pgcpp::parser::RTEKind;
using pgcpp::transaction::HeapTuple;
using pgcpp::transaction::HeapTupleData;
using pgcpp::transaction::ItemPointerData;

void LockRowsState::ExecInit() {
    auto* lrplan = static_cast<LockRows*>(plan);
    lr_lockStrength = lrplan->lockStrength;

    // Open the target relation from the range table.
    int relid_index = lrplan->lockRelid - 1;  // convert 1-based to 0-based
    if (relid_index < 0 || relid_index >= static_cast<int>(state->es_range_table.size())) {
        ereport(pgcpp::error::LogLevel::kError,
                "LockRows: invalid lockRelid " + std::to_string(lrplan->lockRelid));
    }
    RangeTblEntry* rte = state->es_range_table[relid_index];
    if (rte->rtekind != RTEKind::kRelation) {
        ereport(pgcpp::error::LogLevel::kError, "LockRows: target is not a relation");
    }
    lr_relation = RelationOpen(static_cast<pgcpp::catalog::Oid>(rte->relid));
    if (lr_relation == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "LockRows: relation not found");
    }
    state->es_open_relations.push_back(lr_relation);

    // Create result tuple slot from target list.
    auto* result_desc = BuildTupleDescFromTargetList(lrplan->targetlist);
    ps_ResultTupleSlot = TupleTableSlot::Make(result_desc);
    state->es_tupleTable.push_back(ps_ResultTupleSlot);

    ps_ExprContext = CreateExprContext();
}

TupleTableSlot* LockRowsState::ExecProcNode() {
    if (leftps == nullptr) {
        return nullptr;
    }

    for (;;) {
        TupleTableSlot* child_slot = leftps->ExecProcNode();
        if (child_slot == nullptr) {
            return nullptr;
        }

        ResetExprContext(ps_ExprContext);
        ps_ExprContext->ecxt_scantuple = child_slot;

        // Get the TID from the backing heap tuple.
        // Follow the same pattern as ModifyTable: check tts_tuple first,
        // then fall back to the child's scan tuple slot.
        HeapTuple scan_tuple = child_slot->tts_tuple;
        if (scan_tuple == nullptr && leftps->ps_ExprContext != nullptr) {
            TupleTableSlot* scan_slot = leftps->ps_ExprContext->ecxt_scantuple;
            if (scan_slot != nullptr) {
                scan_tuple = scan_slot->tts_tuple;
            }
        }

        if (scan_tuple != nullptr) {
            ItemPointerData tid = scan_tuple->t_self;
            heap_lock_tuple(lr_relation, tid, lr_lockStrength);
        }

        // Apply qual filter if present.
        if (plan->qual != nullptr && !ExecQual(plan->qual, ps_ExprContext)) {
            continue;
        }

        // Project through the target list.
        ExecProject(plan->targetlist, ps_ExprContext, ps_ResultTupleSlot);
        return ps_ResultTupleSlot;
    }
}

void LockRowsState::ExecEnd() {
    // Child cleanup is handled by the outer ExecEndNode recursion.
    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
    lr_relation = nullptr;
}

void LockRowsState::ExecReScan() {
    if (leftps != nullptr) {
        leftps->ExecReScan();
    }
}

}  // namespace pgcpp::executor

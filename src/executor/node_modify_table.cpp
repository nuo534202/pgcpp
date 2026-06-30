// node_modify_table.cpp — DML node implementation (INSERT/UPDATE/DELETE).
//
// Converted from PostgreSQL 15's src/backend/executor/nodeModifyTable.c.
//
// The ModifyTable node performs write operations on a target table.
// It consumes tuples from its child plan (which provides the rows to
// insert, or the TIDs of rows to update/delete) and applies the
// corresponding heap operation.
//
// For INSERT: the child produces tuples matching the target table's
//   schema; each is inserted via heap_insert.
// For DELETE: the child produces tuples from the target table; each
//   tuple's TID is used to delete via heap_delete.
// For UPDATE: the child produces new tuple values; each is used to
//   update the corresponding row via heap_update.
#include "executor/node_modify_table.hpp"

#include <new>

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

using pgcpp::access::heap_delete;
using pgcpp::access::heap_form_tuple;
using pgcpp::access::heap_freetuple;
using pgcpp::access::heap_insert;
using pgcpp::access::heap_update;
using pgcpp::access::Relation;
using pgcpp::access::RelationClose;
using pgcpp::access::RelationOpen;
using pgcpp::access::TupleDesc;
using pgcpp::memory::palloc;
using pgcpp::parser::CmdType;
using pgcpp::parser::RangeTblEntry;
using pgcpp::transaction::HeapTuple;
using pgcpp::transaction::HeapTupleData;
using pgcpp::transaction::ItemPointerData;

void ModifyTableState::ExecInit() {
    auto* mtplan = static_cast<ModifyTable*>(plan);
    mt_operation = mtplan->operation;

    // Look up the target relation from the range table.
    int rtindex = mtplan->resultRelid - 1;  // 1-based to 0-based
    if (rtindex < 0 || rtindex >= static_cast<int>(state->es_range_table.size())) {
        ereport(pgcpp::error::LogLevel::kError, "ModifyTable: invalid resultRelid");
    }
    RangeTblEntry* rte = state->es_range_table[rtindex];

    // Open the target relation.
    mt_relation = RelationOpen(static_cast<pgcpp::catalog::Oid>(rte->relid));
    if (mt_relation == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "ModifyTable: target relation not found");
    }
    state->es_open_relations.push_back(mt_relation);
    mt_tupDesc = mt_relation->rd_att;

    // Create the result slot from the target relation's descriptor.
    ps_ResultTupleSlot = TupleTableSlot::Make(mt_tupDesc);
    state->es_tupleTable.push_back(ps_ResultTupleSlot);

    // Create the expression context.
    ps_ExprContext = CreateExprContext();
    if (leftps != nullptr && leftps->ps_ResultTupleSlot != nullptr) {
        ps_ExprContext->ecxt_scantuple = leftps->ps_ResultTupleSlot;
    }
}

TupleTableSlot* ModifyTableState::ExecProcNode() {
    if (leftps == nullptr)
        return nullptr;

    for (;;) {
        TupleTableSlot* child_slot = leftps->ExecProcNode();
        if (child_slot == nullptr)
            return nullptr;

        ResetExprContext(ps_ExprContext);
        ps_ExprContext->ecxt_scantuple = child_slot;

        switch (mt_operation) {
            case CmdType::kInsert: {
                // Build a heap tuple from the child slot's values.
                HeapTuple tup =
                    heap_form_tuple(mt_tupDesc, child_slot->tts_values, child_slot->tts_isnull);
                heap_insert(mt_relation, tup);
                heap_freetuple(tup);

                // Return the inserted tuple (for RETURNING).
                ps_ResultTupleSlot->StoreVirtual(child_slot->tts_values, child_slot->tts_isnull);
                return ps_ResultTupleSlot;
            }
            case CmdType::kDelete: {
                // The TID of the row to delete comes from the backing heap
                // tuple. The child's result slot is typically a virtual
                // (projected) tuple with tts_tuple == nullptr (ExecProject
                // clears it); in that case, fall back to the child's scan
                // tuple slot, which holds the original heap tuple read from
                // the table and therefore carries the TID in t_self.
                HeapTuple scan_tuple = child_slot->tts_tuple;
                if (scan_tuple == nullptr && leftps->ps_ExprContext != nullptr) {
                    TupleTableSlot* scan_slot = leftps->ps_ExprContext->ecxt_scantuple;
                    if (scan_slot != nullptr) {
                        scan_tuple = scan_slot->tts_tuple;
                    }
                }
                if (scan_tuple == nullptr) {
                    continue;  // no TID available
                }
                ItemPointerData tid = scan_tuple->t_self;
                heap_delete(mt_relation, tid);
                // DELETE without RETURNING produces no output row; continue
                // to the next child tuple. Returning nullptr here would
                // prematurely signal "no more tuples" to ExecutorRun, which
                // would stop after deleting only the first row.
                continue;
            }
            case CmdType::kUpdate: {
                // The child slot has the new values; the old TID comes from
                // the backing heap tuple. As with DELETE, fall back to the
                // child's scan tuple slot when the result slot is virtual.
                HeapTuple scan_tuple = child_slot->tts_tuple;
                if (scan_tuple == nullptr && leftps->ps_ExprContext != nullptr) {
                    TupleTableSlot* scan_slot = leftps->ps_ExprContext->ecxt_scantuple;
                    if (scan_slot != nullptr) {
                        scan_tuple = scan_slot->tts_tuple;
                    }
                }
                if (scan_tuple == nullptr) {
                    continue;
                }
                ItemPointerData otid = scan_tuple->t_self;
                HeapTuple new_tup =
                    heap_form_tuple(mt_tupDesc, child_slot->tts_values, child_slot->tts_isnull);
                heap_update(mt_relation, otid, new_tup);
                heap_freetuple(new_tup);
                // UPDATE without RETURNING produces no output row; continue
                // to the next child tuple (see DELETE comment above).
                continue;
            }
            default:
                return nullptr;
        }
    }
}

void ModifyTableState::ExecEnd() {
    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
    mt_relation = nullptr;
    mt_tupDesc = nullptr;
}

void ModifyTableState::ExecReScan() {
    if (leftps != nullptr) {
        leftps->ExecReScan();
    }
}

}  // namespace pgcpp::executor

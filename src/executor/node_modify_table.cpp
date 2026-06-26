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
#include "mytoydb/executor/node_modify_table.hpp"

#include <new>

#include "mytoydb/access/heapam.hpp"
#include "mytoydb/access/rel.hpp"
#include "mytoydb/common/containers/node.hpp"
#include "mytoydb/common/error/elog.hpp"
#include "mytoydb/executor/estate.hpp"
#include "mytoydb/executor/exec_expr.hpp"
#include "mytoydb/executor/exec_utils.hpp"
#include "mytoydb/executor/plannodes.hpp"
#include "mytoydb/executor/tupletable.hpp"
#include "mytoydb/parser/parsenodes.hpp"
#include "mytoydb/transaction/heap_tuple.hpp"

namespace mytoydb::executor {

using mytoydb::access::heap_delete;
using mytoydb::access::heap_form_tuple;
using mytoydb::access::heap_freetuple;
using mytoydb::access::heap_insert;
using mytoydb::access::heap_update;
using mytoydb::access::Relation;
using mytoydb::access::RelationClose;
using mytoydb::access::RelationOpen;
using mytoydb::access::TupleDesc;
using mytoydb::memory::palloc;
using mytoydb::parser::CmdType;
using mytoydb::parser::RangeTblEntry;
using mytoydb::transaction::HeapTuple;
using mytoydb::transaction::HeapTupleData;
using mytoydb::transaction::ItemPointerData;

void ModifyTableState::ExecInit() {
    auto* mtplan = static_cast<ModifyTable*>(plan);
    mt_operation = mtplan->operation;

    // Look up the target relation from the range table.
    int rtindex = mtplan->resultRelid - 1;  // 1-based to 0-based
    if (rtindex < 0 || rtindex >= static_cast<int>(state->es_range_table.size())) {
        ereport(mytoydb::error::LogLevel::kError, "ModifyTable: invalid resultRelid");
    }
    RangeTblEntry* rte = state->es_range_table[rtindex];

    // Open the target relation.
    mt_relation = RelationOpen(static_cast<mytoydb::catalog::Oid>(rte->relid));
    if (mt_relation == nullptr) {
        ereport(mytoydb::error::LogLevel::kError, "ModifyTable: target relation not found");
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
                // The child slot should have a backing tuple with a TID.
                if (child_slot->tts_tuple == nullptr) {
                    continue;  // no TID available
                }
                ItemPointerData tid = child_slot->tts_tuple->t_self;
                heap_delete(mt_relation, tid);
                return nullptr;  // DELETE doesn't return tuples (unless RETURNING)
            }
            case CmdType::kUpdate: {
                // The child slot has the new values; the old TID comes from
                // the backing tuple.
                if (child_slot->tts_tuple == nullptr) {
                    continue;
                }
                ItemPointerData otid = child_slot->tts_tuple->t_self;
                HeapTuple new_tup =
                    heap_form_tuple(mt_tupDesc, child_slot->tts_values, child_slot->tts_isnull);
                heap_update(mt_relation, otid, new_tup);
                heap_freetuple(new_tup);
                return nullptr;  // UPDATE doesn't return tuples (unless RETURNING)
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

}  // namespace mytoydb::executor

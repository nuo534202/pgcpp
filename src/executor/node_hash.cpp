// node_hash.cpp — Hash table build node implementation.
//
// Converted from PostgreSQL 15's src/backend/executor/nodeHash.c.
//
// The Hash node is the inner child of a HashJoin. It passes tuples from
// its child to the parent HashJoin, which builds the hash table. The
// Hash node itself is mostly a pass-through; the hash table is stored
// in the HashState and managed by the HashJoin.
#include "pgcpp/executor/node_hash.hpp"

#include <new>

#include "pgcpp/executor/estate.hpp"
#include "pgcpp/executor/exec_expr.hpp"
#include "pgcpp/executor/exec_utils.hpp"
#include "pgcpp/executor/plannodes.hpp"
#include "pgcpp/executor/tupletable.hpp"
#include "pgcpp/types/datum.hpp"

namespace mytoydb::executor {

using mytoydb::types::Datum;

void HashTable::Insert(uint64_t hash, TupleTableSlot* slot, Datum keyval, bool keynull) {
    buckets.emplace(hash, HashEntry{slot, keyval, keynull});
}

void HashTable::Clear() {
    buckets.clear();
}

void HashState::ExecInit() {
    // Create the result slot from the target list.
    auto* result_desc = BuildTupleDescFromTargetList(plan->targetlist);
    ps_ResultTupleSlot = TupleTableSlot::Make(result_desc);
    state->es_tupleTable.push_back(ps_ResultTupleSlot);

    // Create the expression context.
    ps_ExprContext = CreateExprContext();
    if (leftps != nullptr && leftps->ps_ResultTupleSlot != nullptr) {
        ps_ExprContext->ecxt_scantuple = leftps->ps_ResultTupleSlot;
    }
}

TupleTableSlot* HashState::ExecProcNode() {
    // The Hash node simply passes tuples from its child to the parent.
    // The parent HashJoin calls this to build the hash table.
    if (leftps == nullptr)
        return nullptr;
    return leftps->ExecProcNode();
}

void HashState::ExecEnd() {
    hashtable.Clear();
    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
}

void HashState::ExecReScan() {
    hashtable.Clear();
    if (leftps != nullptr) {
        leftps->ExecReScan();
    }
}

}  // namespace mytoydb::executor

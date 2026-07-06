// node_hash.cpp — Hash table build node implementation.
//
// Converted from PostgreSQL 15's src/backend/executor/nodeHash.c.
//
// The Hash node is the inner child of a HashJoin. It passes tuples from
// its child to the parent HashJoin, which builds the hash table. The Hash
// node itself is mostly a pass-through; the hash table is stored in the
// HashState and managed by the HashJoin.
//
// With hybrid hash join (P1-4), the HashState also manages batch
// tuplestores for spilled tuples when the inner relation exceeds work_mem.
#include "executor/node_hash.hpp"

#include <new>

#include "common/containers/node.hpp"
#include "executor/estate.hpp"
#include "executor/exec_expr.hpp"
#include "executor/exec_utils.hpp"
#include "executor/plannodes.hpp"
#include "executor/tupletable.hpp"
#include "types/datum.hpp"

namespace pgcpp::executor {

using pgcpp::nodes::destroyPallocNode;
using pgcpp::types::Datum;

void HashTable::Insert(uint64_t hash, TupleTableSlot* slot, Datum keyval, bool keynull) {
    buckets.emplace(hash, HashEntry{slot, keyval, keynull, false});
}

void HashTable::Clear() {
    buckets.clear();
}

void HashState::ExecInit() {
    // Create the result slot from the target list.
    auto* result_desc = BuildTupleDescFromTargetList(plan->targetlist);
    ps_ResultTupleSlot = TupleTableSlot::Make(result_desc);
    state->es_tupleTable.push_back(ps_ResultTupleSlot);
    tupdesc = result_desc;

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

void HashState::ClearHashTable() {
    for (auto& [key, entry] : hashtable.buckets) {
        if (entry.slot != nullptr) {
            destroyPallocNode(entry.slot);
            entry.slot = nullptr;
        }
    }
    hashtable.Clear();
    mem_used = 0;
}

void HashState::ResetBatches() {
    ClearHashTable();
    inner_batches.clear();
    outer_batches.clear();
    num_batches = 1;
    cur_batch = 0;
    mem_used = 0;
}

void HashState::ExpandBatches(int new_num_batches) {
    // Create tuplestores for batches 1..new_num_batches-1.
    inner_batches.resize(new_num_batches - 1);
    for (int i = 0; i < new_num_batches - 1; i++) {
        if (inner_batches[i] == nullptr) {
            inner_batches[i] =
                std::make_unique<pgcpp::sort::Tuplestore>(tupdesc, work_mem, temp_dir);
        }
    }
    // Also create outer batch tuplestores (HashJoin will use them).
    outer_batches.resize(new_num_batches - 1);
    for (int i = 0; i < new_num_batches - 1; i++) {
        if (outer_batches[i] == nullptr) {
            outer_batches[i] = std::make_unique<pgcpp::sort::Tuplestore>(
                outer_tupdesc != nullptr ? outer_tupdesc : tupdesc, work_mem, temp_dir);
        }
    }

    // Redistribute existing in-memory entries: entries whose hash maps to
    // batch != 0 are moved to the appropriate inner batch tuplestore.
    std::vector<std::pair<uint64_t, HashEntry>> to_keep;
    for (auto& [hash, entry] : hashtable.buckets) {
        int batch = static_cast<int>(hash & static_cast<uint64_t>(new_num_batches - 1));
        if (batch == 0) {
            to_keep.emplace_back(hash, std::move(entry));
        } else {
            // Spill this entry to the appropriate batch.
            inner_batches[batch - 1]->PutTuple(entry.slot);
            destroyPallocNode(entry.slot);
            entry.slot = nullptr;
        }
    }
    hashtable.Clear();
    for (auto& [hash, entry] : to_keep) {
        hashtable.buckets.emplace(hash, std::move(entry));
    }

    num_batches = new_num_batches;

    // Recalculate mem_used based on remaining entries (batch 0 only).
    // Each entry is roughly sizeof(HashEntry) + slot overhead.
    mem_used = hashtable.NumEntries() *
               (sizeof(HashEntry) + sizeof(TupleTableSlot) + 8 * (sizeof(Datum) + sizeof(bool)));
}

void HashState::ExecEnd() {
    ResetBatches();
    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
}

void HashState::ExecReScan() {
    ResetBatches();
    if (leftps != nullptr) {
        leftps->ExecReScan();
    }
}

}  // namespace pgcpp::executor

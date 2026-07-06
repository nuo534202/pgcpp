// node_hash.h — Hash table build node state (inner child of HashJoin).
//
// Supports hybrid hash join with batch spilling (P1-4). When the in-memory
// hash table exceeds work_mem, tuples are distributed across multiple
// batches: batch 0 stays in memory, batches 1..N-1 spill to tuplestores
// on disk. The HashJoin node processes batch 0 first, then loads each
// subsequent batch into memory and probes with the corresponding outer
// batch.
#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "executor/node_exec.hpp"
#include "executor/tupletable.hpp"
#include "types/datum.hpp"
#include "utils/sort/tuplestore.hpp"

namespace pgcpp::executor {

// HashEntry — a tuple stored in the hash table.
struct HashEntry {
    TupleTableSlot* slot;
    pgcpp::types::Datum hashkey;
    bool keynull;
    bool matched;  // set true when an outer tuple matches this entry (RIGHT/FULL)
};

// HashTable — the hash table built by the Hash node.
struct HashTable {
    // Key: hash of the join key values. Value: list of matching tuples.
    std::unordered_multimap<uint64_t, HashEntry> buckets;

    void Insert(uint64_t hash, TupleTableSlot* slot, pgcpp::types::Datum keyval, bool keynull);
    void Clear();
    size_t NumEntries() const { return buckets.size(); }
};

class HashState : public PlanState {
public:
    HashState(Plan* p, EState* s) : PlanState(p, s) {}

    HashTable hashtable;
    std::vector<pgcpp::parser::Node*> hashkeys;  // hash key expressions

    // --- Hybrid hash join batch spilling (P1-4) ---
    // work_mem: maximum bytes of in-memory hash table before spilling.
    size_t work_mem = 4 * 1024 * 1024;  // 4MB default
    // num_batches: number of hash batches (power of 2). 1 = no spill.
    int num_batches = 1;
    // cur_batch: the batch currently being processed (0 = in-memory batch).
    int cur_batch = 0;
    // inner_batches: spilled inner tuples, one tuplestore per batch [1..N-1].
    std::vector<std::unique_ptr<pgcpp::sort::Tuplestore>> inner_batches;
    // outer_batches: spilled outer tuples, one tuplestore per batch [1..N-1].
    std::vector<std::unique_ptr<pgcpp::sort::Tuplestore>> outer_batches;
    // mem_used: estimated memory usage of the in-memory hash table.
    size_t mem_used = 0;
    // temp_dir: directory for temp files.
    std::string temp_dir = "/tmp";
    // tupdesc: descriptor for inner tuples (for creating inner batch tuplestores).
    pgcpp::access::TupleDesc tupdesc;
    // outer_tupdesc: descriptor for outer tuples (for creating outer batch tuplestores).
    pgcpp::access::TupleDesc outer_tupdesc;

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;

    // Compute the batch number for a hash value (0..num_batches-1).
    int BatchOfHash(uint64_t hash) const {
        if (num_batches <= 1)
            return 0;
        return static_cast<int>(hash & static_cast<uint64_t>(num_batches - 1));
    }

    // Expand from 1 batch to num_batches, redistributing existing entries.
    // Called when the in-memory hash table exceeds work_mem.
    void ExpandBatches(int new_num_batches);

    // Clear the hash table and free all slots.
    void ClearHashTable();

    // Reset batch state for a new build/probe cycle.
    void ResetBatches();
};

}  // namespace pgcpp::executor

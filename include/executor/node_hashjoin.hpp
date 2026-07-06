// node_hashjoin.h — Hash join node state.
//
// Supports INNER, LEFT, RIGHT, FULL, SEMI, ANTI joins and hybrid hash
// batch spilling (P1-4). When the inner relation exceeds work_mem,
// tuples are distributed across multiple batches: batch 0 stays in
// memory, batches 1..N-1 spill to disk. The join processes batch 0
// first, then loads each subsequent batch into memory and probes with
// the corresponding outer batch.
#pragma once

#include "executor/node_exec.hpp"
#include "executor/node_hash.hpp"

namespace pgcpp::executor {

class HashJoinState : public PlanState {
public:
    HashJoinState(Plan* p, EState* s) : PlanState(p, s) {}

    pgcpp::parser::JoinType hj_jointype = pgcpp::parser::JoinType::kInner;
    std::vector<pgcpp::parser::Node*> hj_hashclauses;

    // Extracted from hashclauses: left (outer) and right (inner) key
    // expressions. In PostgreSQL, ExecInitHashJoin splits each hash clause
    // OpExpr into its left and right args so each side can be evaluated
    // independently during the build and probe phases.
    std::vector<pgcpp::parser::Node*> hj_outer_hashkeys;
    std::vector<pgcpp::parser::Node*> hj_inner_hashkeys;

    // Execution phase.
    // kBuildHashTable: consume inner tuples, build hash table / spill batches.
    // kProbeHashTable: probe with outer tuples (batch 0 or current batch).
    // kScanUnmatched: scan hash table for unmatched inner (RIGHT/FULL).
    // kNextBatch: load next spilled batch into memory.
    // kDone: no more output.
    enum class Phase { kBuildHashTable, kProbeHashTable, kScanUnmatched, kNextBatch, kDone };
    Phase hj_phase = Phase::kBuildHashTable;

    // Build side: the Hash node (right child).
    HashState* hj_HashState = nullptr;

    // Probe side: the outer (left) child.
    TupleTableSlot* hj_OuterTupleSlot = nullptr;
    bool hj_NeedNewOuter = true;
    // true if the current outer tuple has produced at least one output row
    // (i.e. found a matching inner tuple that passed the join qual). Used by
    // LEFT JOIN to decide whether to emit a NULL-padded row when the bucket
    // is exhausted, and by SEMI/ANTI to decide whether to emit/skip.
    bool hj_MatchedOuter = false;

    // Current bucket iterator for matching tuples.
    typename std::unordered_multimap<uint64_t, HashEntry>::iterator hj_curBucket;
    typename std::unordered_multimap<uint64_t, HashEntry>::iterator hj_curBucketEnd;
    bool hj_hasBucket = false;

    // For RIGHT/FULL join: after probing all outer tuples for a batch,
    // scan the hash table for unmatched inner tuples and emit them with
    // NULL outer columns.
    typename std::unordered_multimap<uint64_t, HashEntry>::iterator hj_scanIter;
    bool hj_scanStarted = false;

    // For batch processing: the current batch being probed.
    // Batch 0 is the in-memory batch (processed first). Batches 1..N-1
    // are spilled to disk and processed one at a time.
    int hj_cur_batch = 0;

    // Slot for reading tuples from outer batch tuplestores.
    TupleTableSlot* hj_BatchOuterSlot = nullptr;

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;

private:
    // Build the hash table from the inner (right) child. Handles batch
    // spilling when work_mem is exceeded.
    void BuildHashTable();

    // Compute the hash of the outer join keys for the current outer tuple.
    // Returns the hash value, or sets *any_null = true if any key is NULL.
    uint64_t HashOuterKeys(bool* any_null);

    // Compute the hash of the inner join keys for the given slot.
    uint64_t HashInnerKeys(TupleTableSlot* slot, bool* any_null);

    // Probe the hash table with the current outer tuple. Returns a result
    // slot if a match is found, or nullptr if no match (caller decides what
    // to do based on join type).
    TupleTableSlot* ProbeHashTable(uint64_t hash);

    // Emit a row with NULL inner columns (for LEFT/FULL join unmatched outer).
    TupleTableSlot* EmitNullInnerRow();

    // Emit a row with NULL outer columns (for RIGHT/FULL join unmatched inner).
    TupleTableSlot* EmitNullOuterRow(TupleTableSlot* inner);

    // Scan the hash table for unmatched inner tuples (RIGHT/FULL).
    // Returns a result slot or nullptr when scan is complete.
    TupleTableSlot* ScanUnmatched();

    // Load the next spilled batch into the hash table.
    // Returns true if a batch was loaded, false if no more batches.
    bool LoadNextBatch();

    // Probe the current batch using outer tuples from the batch tuplestore.
    // Returns a result slot or nullptr when the batch is exhausted.
    TupleTableSlot* ProbeBatch();
};

}  // namespace pgcpp::executor

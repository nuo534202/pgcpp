// node_hashjoin.h — Hash join node state.
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
    enum class Phase { kBuildHashTable, kProbeHashTable, kDone };
    Phase hj_phase = Phase::kBuildHashTable;

    // Build side: the Hash node (right child).
    HashState* hj_HashState = nullptr;

    // Probe side: the outer (left) child.
    TupleTableSlot* hj_OuterTupleSlot = nullptr;
    bool hj_NeedNewOuter = true;
    // true if the current outer tuple has produced at least one output row
    // (i.e. found a matching inner tuple that passed the join qual). Used by
    // LEFT JOIN to decide whether to emit a NULL-padded row when the bucket
    // is exhausted.
    bool hj_MatchedOuter = false;

    // Current bucket iterator for matching tuples.
    typename std::unordered_multimap<uint64_t, HashEntry>::iterator hj_curBucket;
    typename std::unordered_multimap<uint64_t, HashEntry>::iterator hj_curBucketEnd;
    bool hj_hasBucket = false;

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;
};

}  // namespace pgcpp::executor

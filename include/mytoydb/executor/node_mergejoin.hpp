// node_mergejoin.h — MergeJoin node state (join on sorted inputs).
//
// Both children must be sorted on the merge keys. Advances through
// matching runs, emitting a joined tuple for each outer-inner pair
// within a run. Supports INNER and LEFT joins.
#pragma once

#include <vector>

#include "mytoydb/executor/node_exec.hpp"

namespace mytoydb::executor {

class MergeJoinState : public PlanState {
public:
    MergeJoinState(Plan* p, EState* s) : PlanState(p, s) {}

    mytoydb::parser::JoinType mj_jointype = mytoydb::parser::JoinType::kInner;
    std::vector<int> mj_clauses;     // 1-based outer attr for each merge clause
    std::vector<int> mj_inner_cols;  // 1-based inner attr for each merge clause
    TupleTableSlot* mj_OuterTupleSlot = nullptr;
    TupleTableSlot* mj_InnerTupleSlot = nullptr;
    bool mj_NeedNewOuter = true;
    bool mj_NeedNewInner = true;
    bool mj_MatchedOuter = false;

    // Buffer of matching inner tuples for the current outer key group.
    std::vector<TupleTableSlot*> mj_buffer;
    size_t mj_buffer_index = 0;

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;

private:
    bool TuplesMatch(TupleTableSlot* outer, TupleTableSlot* inner);
};

}  // namespace mytoydb::executor

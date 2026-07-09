// node_mergeappend.h — MergeAppend node state.
//
// Merges multiple sorted child plans into a single sorted output stream
// using a k-way merge. Each child must produce tuples sorted on the merge
// key columns (sortColIdx). A simple selection-based merge is used: on each
// call we pick the smallest current head tuple among the children and
// return it, then advance that child.
//
// Comparison honours sortOperators, nullsFirst, and reverse (DESC) per
// column. Only the int4/int8/float8/text comparison helpers from
// exec_utils are reused via CompareDatumValues.
#pragma once

#include <vector>

#include "executor/node_exec.hpp"

namespace pgcpp::executor {

class MergeAppendState : public PlanState {
public:
    MergeAppendState(Plan* p, EState* s) : PlanState(p, s) {}

    std::vector<PlanState*> merge_ps;  // child plan states
    std::vector<int> sortColIdx;       // 1-based attr numbers to sort by
    std::vector<bool> nullsFirst;      // NULLS FIRST/LAST per column
    std::vector<bool> reverse;         // DESC per column

    // Per-child current head tuple (cached). nullptr means the child is
    // exhausted or no tuple has been fetched yet for the current round.
    std::vector<TupleTableSlot*> head_slots;
    std::vector<bool> child_done;

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;

private:
    // Compare two slots on the merge keys. Returns -1/0/1 accounting for
    // nullsFirst and reverse.
    int CompareHeads(const TupleTableSlot* a, const TupleTableSlot* b);
};

}  // namespace pgcpp::executor

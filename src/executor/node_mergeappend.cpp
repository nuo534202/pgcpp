// node_mergeappend.cpp — MergeAppend node implementation.
//
// Merges multiple sorted child plans into one sorted output stream via a
// k-way selection merge. On each call we scan the per-child head tuple
// cache, pick the smallest head according to the merge keys (honouring
// NULLS FIRST/LAST and ASC/DESC), emit it, and advance that child.
//
// Children are stored in merge_plans (not lefttree/righttree), so like
// Append we initialise them ourselves in ExecInit.
#include "executor/node_mergeappend.hpp"

#include "common/containers/node.hpp"
#include "executor/estate.hpp"
#include "executor/exec_expr.hpp"
#include "executor/exec_utils.hpp"
#include "executor/plannodes.hpp"
#include "executor/tupletable.hpp"

namespace pgcpp::executor {

using pgcpp::catalog::Oid;
using pgcpp::types::kInt4Oid;

void MergeAppendState::ExecInit() {
    auto* maplan = static_cast<MergeAppend*>(plan);
    sortColIdx = maplan->sortColIdx;
    nullsFirst = maplan->nullsFirst;
    reverse = maplan->reverse;

    auto* result_desc = BuildTupleDescFromTargetList(plan->targetlist);
    ps_ResultTupleSlot = TupleTableSlot::Make(result_desc);
    state->es_tupleTable.push_back(ps_ResultTupleSlot);
    ps_ExprContext = CreateExprContext();

    // Initialise child plan states (children are in merge_plans, not
    // lefttree/righttree — see Append for the same pattern).
    for (Plan* child_plan : maplan->merge_plans) {
        if (child_plan != nullptr) {
            PlanState* child_ps = ExecInitNode(child_plan, state);
            merge_ps.push_back(child_ps);
        }
    }

    // Allocate one head slot per child, sharing the child's tuple
    // descriptor so comparisons see the correct column types.
    for (PlanState* child : merge_ps) {
        TupleTableSlot* head = nullptr;
        if (child != nullptr && child->ps_ResultTupleSlot != nullptr) {
            head = TupleTableSlot::Make(child->ps_ResultTupleSlot->tts_tupleDescriptor);
            state->es_tupleTable.push_back(head);
        }
        head_slots.push_back(head);
        child_done.push_back(false);
    }
}

int MergeAppendState::CompareHeads(const TupleTableSlot* a, const TupleTableSlot* b) {
    if (a == nullptr || b == nullptr)
        return 0;
    const auto* tupdesc = a->tts_tupleDescriptor;
    for (size_t i = 0; i < sortColIdx.size(); i++) {
        int attno = sortColIdx[i];
        if (attno < 1)
            continue;
        int idx = attno - 1;
        bool a_null = (idx < a->Natts()) ? a->tts_isnull[idx] : true;
        bool b_null = (idx < b->Natts()) ? b->tts_isnull[idx] : true;
        Oid typid =
            (tupdesc != nullptr && idx < tupdesc->natts) ? tupdesc->attrs[idx].atttypid : kInt4Oid;

        int cmp;
        if (a_null && b_null) {
            cmp = 0;
        } else if (a_null) {
            // NULLs first => a < b; otherwise a > b.
            bool nf = (i < nullsFirst.size()) ? nullsFirst[i] : false;
            cmp = nf ? -1 : 1;
        } else if (b_null) {
            bool nf = (i < nullsFirst.size()) ? nullsFirst[i] : false;
            cmp = nf ? 1 : -1;
        } else {
            cmp = CompareDatumValues(a->tts_values[idx], false, b->tts_values[idx], false, typid);
        }

        if (cmp != 0) {
            // Reverse for DESC.
            bool rev = (i < reverse.size()) ? reverse[i] : false;
            return rev ? -cmp : cmp;
        }
    }
    return 0;
}

TupleTableSlot* MergeAppendState::ExecProcNode() {
    for (;;) {
        // Ensure every non-exhausted child has a head tuple cached.
        for (size_t i = 0; i < merge_ps.size(); i++) {
            if (child_done[i] || head_slots[i] == nullptr)
                continue;
            if (!head_slots[i]->tts_isempty)
                continue;  // already has a cached tuple
            TupleTableSlot* child_slot = merge_ps[i]->ExecProcNode();
            if (child_slot == nullptr) {
                child_done[i] = true;
                head_slots[i]->Clear();
            } else {
                head_slots[i]->StoreVirtual(child_slot->tts_values, child_slot->tts_isnull);
            }
        }

        // Find the smallest head among the active children.
        int best = -1;
        for (size_t i = 0; i < merge_ps.size(); i++) {
            if (child_done[i] || head_slots[i] == nullptr || head_slots[i]->tts_isempty)
                continue;
            if (best < 0) {
                best = static_cast<int>(i);
            } else {
                int cmp = CompareHeads(head_slots[i], head_slots[best]);
                if (cmp < 0)
                    best = static_cast<int>(i);
            }
        }

        if (best < 0)
            return nullptr;  // all children exhausted

        // Emit the chosen head, then mark its slot empty so the next call
        // advances that child.
        TupleTableSlot* chosen = head_slots[best];
        ResetExprContext(ps_ExprContext);
        int natts = ps_ResultTupleSlot->Natts();
        int src_natts = chosen->Natts();
        int ncopy = natts < src_natts ? natts : src_natts;
        for (int i = 0; i < ncopy; i++) {
            ps_ResultTupleSlot->tts_values[i] = chosen->tts_values[i];
            ps_ResultTupleSlot->tts_isnull[i] = chosen->tts_isnull[i];
        }
        ps_ResultTupleSlot->tts_nvalid = true;
        ps_ResultTupleSlot->tts_isempty = false;

        chosen->Clear();  // advance next time
        return ps_ResultTupleSlot;
    }
}

void MergeAppendState::ExecEnd() {
    for (PlanState* child : merge_ps) {
        ExecEndNode(child);
    }
    merge_ps.clear();
    head_slots.clear();
    child_done.clear();

    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
}

void MergeAppendState::ExecReScan() {
    for (size_t i = 0; i < merge_ps.size(); i++) {
        child_done[i] = false;
        if (head_slots[i] != nullptr)
            head_slots[i]->Clear();
        if (merge_ps[i] != nullptr)
            merge_ps[i]->ExecReScan();
    }
}

}  // namespace pgcpp::executor

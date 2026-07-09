// node_exec.cpp — PlanState dispatch: ExecInitNode and ExecEndNode.
//
// Converted from PostgreSQL 15's src/backend/executor/execProcnode.c.
//
// ExecInitNode dispatches on PlanType to create the matching PlanState
// subclass, recursively initializing child nodes. ExecEndNode recursively
// tears down a node and its children.
#include "executor/node_exec.hpp"

#include <new>

#include "common/containers/node.hpp"
#include "common/memory/memory_context.hpp"
#include "executor/estate.hpp"
#include "executor/exec_expr.hpp"
#include "executor/exec_utils.hpp"
#include "executor/node_agg.hpp"
#include "executor/node_append.hpp"
#include "executor/node_bitmap_heapscan.hpp"
#include "executor/node_bitmap_indexscan.hpp"
#include "executor/node_ctescan.hpp"
#include "executor/node_functionscan.hpp"
#include "executor/node_gather.hpp"
#include "executor/node_group.hpp"
#include "executor/node_hash.hpp"
#include "executor/node_hashjoin.hpp"
#include "executor/node_incrementalsort.hpp"
#include "executor/node_indexscan.hpp"
#include "executor/node_limit.hpp"
#include "executor/node_lockrows.hpp"
#include "executor/node_material.hpp"
#include "executor/node_memoize.hpp"
#include "executor/node_mergeappend.hpp"
#include "executor/node_mergejoin.hpp"
#include "executor/node_modify_table.hpp"
#include "executor/node_nestloop.hpp"
#include "executor/node_projectset.hpp"
#include "executor/node_recursiveunion.hpp"
#include "executor/node_seqscan.hpp"
#include "executor/node_setop.hpp"
#include "executor/node_sort.hpp"
#include "executor/node_subqueryscan.hpp"
#include "executor/node_tidscan.hpp"
#include "executor/node_unique.hpp"
#include "executor/node_valuesscan.hpp"
#include "executor/node_windowagg.hpp"
#include "executor/node_worktablescan.hpp"
#include "executor/tupletable.hpp"

namespace pgcpp::executor {
using pgcpp::nodes::destroyPallocNode;
using pgcpp::nodes::makePallocNode;

using pgcpp::memory::palloc;
using pgcpp::memory::pfree;

// PlanState destructor — defined here (declared pure-virtual-body in header).
// The base destructor does nothing; subclass destructors handle cleanup.
PlanState::~PlanState() = default;

namespace {

// Create a PlanState subclass for the given plan, without initializing
// children. Dispatches on plan->type.
PlanState* CreatePlanState(Plan* plan, EState* state) {
    switch (plan->type) {
        case PlanType::kResult: {
            return makePallocNode<ResultState>(plan, state);
        }
        case PlanType::kSeqScan: {
            return makePallocNode<SeqScanState>(plan, state);
        }
        case PlanType::kIndexScan: {
            return makePallocNode<IndexScanState>(plan, state);
        }
        case PlanType::kAgg: {
            return makePallocNode<AggState>(plan, state);
        }
        case PlanType::kSort: {
            return makePallocNode<SortState>(plan, state);
        }
        case PlanType::kNestLoop: {
            return makePallocNode<NestLoopState>(plan, state);
        }
        case PlanType::kHashJoin: {
            return makePallocNode<HashJoinState>(plan, state);
        }
        case PlanType::kHash: {
            return makePallocNode<HashState>(plan, state);
        }
        case PlanType::kModifyTable: {
            return makePallocNode<ModifyTableState>(plan, state);
        }
        // --- Task 15.14: P1/P2 executor nodes ---
        case PlanType::kLimit: {
            return makePallocNode<LimitState>(plan, state);
        }
        case PlanType::kAppend: {
            return makePallocNode<AppendState>(plan, state);
        }
        case PlanType::kMaterial: {
            return makePallocNode<MaterialState>(plan, state);
        }
        case PlanType::kUnique: {
            return makePallocNode<UniqueState>(plan, state);
        }
        case PlanType::kSubqueryScan: {
            return makePallocNode<SubqueryScanState>(plan, state);
        }
        case PlanType::kMergeJoin: {
            return makePallocNode<MergeJoinState>(plan, state);
        }
        case PlanType::kCteScan: {
            return makePallocNode<CteScanState>(plan, state);
        }
        case PlanType::kWindowAgg: {
            return makePallocNode<WindowAggState>(plan, state);
        }
        // --- P1-7: missing executor nodes (batch 1) ---
        case PlanType::kGroup: {
            return makePallocNode<GroupState>(plan, state);
        }
        case PlanType::kSetOp: {
            return makePallocNode<SetOpState>(plan, state);
        }
        case PlanType::kMergeAppend: {
            return makePallocNode<MergeAppendState>(plan, state);
        }
        case PlanType::kBitmapIndexScan: {
            return makePallocNode<BitmapIndexScanState>(plan, state);
        }
        case PlanType::kBitmapHeapScan: {
            return makePallocNode<BitmapHeapScanState>(plan, state);
        }
        case PlanType::kLockRows: {
            return makePallocNode<LockRowsState>(plan, state);
        }
        // --- P2-2: missing executor nodes (batch 2) ---
        case PlanType::kValuesScan: {
            return makePallocNode<ValuesScanState>(plan, state);
        }
        case PlanType::kTidScan: {
            return makePallocNode<TidScanState>(plan, state);
        }
        case PlanType::kFunctionScan: {
            return makePallocNode<FunctionScanState>(plan, state);
        }
        case PlanType::kProjectSet: {
            return makePallocNode<ProjectSetState>(plan, state);
        }
        case PlanType::kMemoize: {
            return makePallocNode<MemoizeState>(plan, state);
        }
        case PlanType::kIncrementalSort: {
            return makePallocNode<IncrementalSortState>(plan, state);
        }
        case PlanType::kRecursiveUnion: {
            return makePallocNode<RecursiveUnionState>(plan, state);
        }
        case PlanType::kWorkTableScan: {
            return makePallocNode<WorkTableScanState>(plan, state);
        }
        case PlanType::kGather: {
            return makePallocNode<GatherState>(plan, state);
        }
        case PlanType::kGatherMerge: {
            return makePallocNode<GatherMergeState>(plan, state);
        }
    }
    return nullptr;
}

}  // namespace

PlanState* ExecInitNode(Plan* plan, EState* state) {
    if (plan == nullptr)
        return nullptr;

    PlanState* ps = CreatePlanState(plan, state);
    if (ps == nullptr)
        return nullptr;

    // Recursively initialize children.
    if (plan->lefttree != nullptr) {
        ps->leftps = ExecInitNode(plan->lefttree, state);
    }
    if (plan->righttree != nullptr) {
        ps->rightps = ExecInitNode(plan->righttree, state);
    }

    // Initialize this node (open relations, create scan descriptors, etc.).
    ps->ExecInit();
    return ps;
}

void ExecEndNode(PlanState* node) {
    if (node == nullptr)
        return;

    // End this node (node-specific cleanup: close scans, relations, etc.).
    node->ExecEnd();

    // Recurse on children.
    ExecEndNode(node->leftps);
    ExecEndNode(node->rightps);
    node->leftps = nullptr;
    node->rightps = nullptr;

    // Free the node.
    destroyPallocNode(node);
}

// --- ResultState implementation ---

void ResultState::ExecInit() {
    auto* result_desc = BuildTupleDescFromTargetList(plan->targetlist);
    ps_ResultTupleSlot = TupleTableSlot::Make(result_desc);
    state->es_tupleTable.push_back(ps_ResultTupleSlot);
    ps_ExprContext = CreateExprContext();
    rs_done = false;
}

TupleTableSlot* ResultState::ExecProcNode() {
    if (rs_done)
        return nullptr;
    rs_done = true;
    ResetExprContext(ps_ExprContext);
    ExecProject(plan->targetlist, ps_ExprContext, ps_ResultTupleSlot);
    return ps_ResultTupleSlot;
}

void ResultState::ExecEnd() {
    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
}

}  // namespace pgcpp::executor

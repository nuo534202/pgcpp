// node_exec.cpp — PlanState dispatch: ExecInitNode and ExecEndNode.
//
// Converted from PostgreSQL 15's src/backend/executor/execProcnode.c.
//
// ExecInitNode dispatches on PlanType to create the matching PlanState
// subclass, recursively initializing child nodes. ExecEndNode recursively
// tears down a node and its children.
#include "pgcpp/executor/node_exec.hpp"

#include <new>

#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/common/memory/memory_context.hpp"
#include "pgcpp/executor/estate.hpp"
#include "pgcpp/executor/exec_expr.hpp"
#include "pgcpp/executor/exec_utils.hpp"
#include "pgcpp/executor/node_agg.hpp"
#include "pgcpp/executor/node_append.hpp"
#include "pgcpp/executor/node_ctescan.hpp"
#include "pgcpp/executor/node_hash.hpp"
#include "pgcpp/executor/node_hashjoin.hpp"
#include "pgcpp/executor/node_indexscan.hpp"
#include "pgcpp/executor/node_limit.hpp"
#include "pgcpp/executor/node_material.hpp"
#include "pgcpp/executor/node_mergejoin.hpp"
#include "pgcpp/executor/node_modify_table.hpp"
#include "pgcpp/executor/node_nestloop.hpp"
#include "pgcpp/executor/node_seqscan.hpp"
#include "pgcpp/executor/node_sort.hpp"
#include "pgcpp/executor/node_subqueryscan.hpp"
#include "pgcpp/executor/node_unique.hpp"
#include "pgcpp/executor/node_windowagg.hpp"
#include "pgcpp/executor/tupletable.hpp"

namespace mytoydb::executor {
using mytoydb::nodes::destroyPallocNode;
using mytoydb::nodes::makePallocNode;

using mytoydb::memory::palloc;
using mytoydb::memory::pfree;

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

}  // namespace mytoydb::executor

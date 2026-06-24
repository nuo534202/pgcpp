// node_exec.cpp — PlanState dispatch: ExecInitNode and ExecEndNode.
//
// Converted from PostgreSQL 15's src/backend/executor/execProcnode.c.
//
// ExecInitNode dispatches on PlanType to create the matching PlanState
// subclass, recursively initializing child nodes. ExecEndNode recursively
// tears down a node and its children.
#include "mytoydb/executor/node_exec.h"

#include <new>

#include "mytoydb/common/memory/memory_context.h"
#include "mytoydb/executor/estate.h"
#include "mytoydb/executor/exec_expr.h"
#include "mytoydb/executor/exec_utils.h"
#include "mytoydb/executor/node_agg.h"
#include "mytoydb/executor/node_hash.h"
#include "mytoydb/executor/node_hashjoin.h"
#include "mytoydb/executor/node_indexscan.h"
#include "mytoydb/executor/node_modify_table.h"
#include "mytoydb/executor/node_nestloop.h"
#include "mytoydb/executor/node_seqscan.h"
#include "mytoydb/executor/node_sort.h"
#include "mytoydb/executor/tupletable.h"

namespace mytoydb::executor {

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
            void* mem = palloc(sizeof(ResultState));
            return new (mem) ResultState(plan, state);
        }
        case PlanType::kSeqScan: {
            void* mem = palloc(sizeof(SeqScanState));
            return new (mem) SeqScanState(plan, state);
        }
        case PlanType::kIndexScan: {
            void* mem = palloc(sizeof(IndexScanState));
            return new (mem) IndexScanState(plan, state);
        }
        case PlanType::kAgg: {
            void* mem = palloc(sizeof(AggState));
            return new (mem) AggState(plan, state);
        }
        case PlanType::kSort: {
            void* mem = palloc(sizeof(SortState));
            return new (mem) SortState(plan, state);
        }
        case PlanType::kNestLoop: {
            void* mem = palloc(sizeof(NestLoopState));
            return new (mem) NestLoopState(plan, state);
        }
        case PlanType::kHashJoin: {
            void* mem = palloc(sizeof(HashJoinState));
            return new (mem) HashJoinState(plan, state);
        }
        case PlanType::kHash: {
            void* mem = palloc(sizeof(HashState));
            return new (mem) HashState(plan, state);
        }
        case PlanType::kModifyTable: {
            void* mem = palloc(sizeof(ModifyTableState));
            return new (mem) ModifyTableState(plan, state);
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
    node->~PlanState();
    pfree(node);
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

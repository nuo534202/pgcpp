// node_exec.h — PlanState base class and node dispatch.
//
// Converted from PostgreSQL 15's src/include/executor/execdesc.h and
// src/backend/executor/execProcnode.c.
//
// PlanState is the base class for per-node executor runtime state.
// Each Plan node type has a corresponding PlanState subclass that
// holds the runtime state (scan descriptors, hash tables, etc.) and
// implements the ExecProcNode virtual method.
//
// ExecInitNode dispatches on PlanType to create the matching PlanState.
// ExecProcNode is the main execution loop driver: calling it returns
// the next result tuple (as a TupleTableSlot*) or NULL when exhausted.
#pragma once

#include "mytoydb/executor/estate.hpp"
#include "mytoydb/executor/exec_expr.hpp"
#include "mytoydb/executor/plannodes.hpp"
#include "mytoydb/executor/tupletable.hpp"

namespace mytoydb::executor {

// PlanState — base class for per-node executor state.
class PlanState {
public:
    Plan* plan = nullptr;
    EState* state = nullptr;
    PlanState* leftps = nullptr;
    PlanState* rightps = nullptr;
    TupleTableSlot* ps_ResultTupleSlot = nullptr;
    ExprContext* ps_ExprContext = nullptr;

    PlanState(Plan* p, EState* s) : plan(p), state(s) {}
    virtual ~PlanState();

    // Initialize the node (open relations, create scan descriptors, etc.).
    virtual void ExecInit() = 0;

    // Return the next result tuple, or nullptr when exhausted.
    virtual TupleTableSlot* ExecProcNode() = 0;

    // Clean up (close relations, free scan descriptors, etc.).
    virtual void ExecEnd() = 0;

    // Rescan the node (reset to beginning).
    virtual void ExecReScan() {}
};

// ResultState — for queries with no FROM clause (e.g., SELECT 1).
// Produces a single tuple from the target list (evaluating constants),
// then returns nullptr on subsequent calls.
class ResultState : public PlanState {
public:
    ResultState(Plan* p, EState* s) : PlanState(p, s) {}

    bool rs_done = false;

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override { rs_done = false; }
};

// ExecInitNode — create a PlanState for the given Plan.
// Recursively initializes child nodes.
PlanState* ExecInitNode(Plan* plan, EState* state);

// ExecProcNode — wrapper that calls node->ExecProcNode().
// Provided for PostgreSQL API compatibility.
inline TupleTableSlot* ExecProcNode(PlanState* node) {
    return node->ExecProcNode();
}

// ExecEndNode — clean up a node and its children.
void ExecEndNode(PlanState* node);

}  // namespace mytoydb::executor

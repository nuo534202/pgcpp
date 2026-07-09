// parallel.cpp — Parallel query framework (single-process stub).
//
// Converted from PostgreSQL 15's src/backend/executor/execParallel.c.
//
// See parallel.h for the design rationale. All functions here are serial
// stubs: parallel mode is tracked but no workers are ever launched.
#include "executor/parallel.hpp"

#include "common/containers/node.hpp"
#include "common/memory/memory_context.hpp"

namespace pgcpp::executor {

using pgcpp::nodes::destroyPallocNode;
using pgcpp::nodes::makePallocNode;

namespace {

// Backend-local flag tracking whether parallel mode is active. In PostgreSQL
// this lives in the backend's CurrentTransactionState; the serial stub uses
// a process-global flag since there is never more than one active worker.
bool g_in_parallel_mode = false;

}  // namespace

void EnterParallelMode() {
    g_in_parallel_mode = true;
}

void ExitParallelMode() {
    g_in_parallel_mode = false;
}

bool IsInParallelMode() {
    return g_in_parallel_mode;
}

int LaunchParallelWorkers(ParallelContext* pc) {
    if (pc == nullptr)
        return 0;
    // Serial stub: never launch workers. The leader executes the child plan
    // directly (the nworkers = 0 fallback path in Gather/GatherMerge).
    pc->nworkers_launched = 0;
    return 0;
}

ParallelContext* CreateParallelContext(int nworkers) {
    auto* pc = makePallocNode<ParallelContext>();
    pc->nworkers_requested = nworkers;
    pc->nworkers_launched = 0;
    pc->parallel_mode = false;
    return pc;
}

void DestroyParallelContext(ParallelContext* pc) {
    if (pc == nullptr)
        return;
    // ParallelContext is a plain struct with no C++ members needing cleanup;
    // we still go through destroyPallocNode to keep the allocator consistent.
    destroyPallocNode(pc);
}

}  // namespace pgcpp::executor

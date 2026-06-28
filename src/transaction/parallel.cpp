// parallel.cpp — Parallel query infrastructure stub.
//
// Converted from PostgreSQL 15's src/backend/executor/execParallel.cpp and
// src/include/storage/parallel.h.
//
// PostgreSQL supports parallel query via worker processes that share memory
// and cooperate on query execution. MyToyDB is single-process and does not
// implement parallel workers, but we provide the API surface so that
// higher-level modules (executor, planner) can compile against it.
#include "pgcpp/transaction/parallel.hpp"

namespace mytoydb::transaction {

void InitializeParallelInfrastructure() {
    // No-op in MyToyDB (no parallel workers).
}

ParallelContext* CreateParallelContext(int nworkers) {
    // Keep the requested nworkers for API compatibility, but initialized
    // stays false until LaunchParallelWorkers is called.
    return new ParallelContext{nworkers, false};
}

void DestroyParallelContext(ParallelContext* ctx) {
    delete ctx;
}

int LaunchParallelWorkers(ParallelContext* ctx) {
    // No workers are actually launched; mark the context as initialized so
    // callers can proceed sequentially.
    ctx->initialized = true;
    return 0;
}

}  // namespace mytoydb::transaction

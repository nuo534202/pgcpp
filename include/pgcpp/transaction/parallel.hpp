// parallel.h — Parallel query infrastructure stub.
//
// Converted from PostgreSQL 15's src/include/storage/parallel.h.
//
// PostgreSQL supports parallel query via worker processes that share memory
// and cooperate on query execution. MyToyDB is single-process and does not
// implement parallel workers, but we provide the API surface so that
// higher-level modules (executor, planner) can compile against it.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mytoydb::transaction {

// ParallelContext — opaque handle (unused in MyToyDB).
struct ParallelContext {
    int nworkers = 0;
    bool initialized = false;
};

// InitializeParallelInfrastructure — set up the parallel subsystem (no-op).
void InitializeParallelInfrastructure();

// ParallelContextActive — false in MyToyDB (no parallel workers).
inline bool ParallelContextActive() {
    return false;
}

// CreateParallelContext — create a context for `nworkers` workers.
// In MyToyDB, returns a context with nworkers=0 (no actual parallelism).
ParallelContext* CreateParallelContext(int nworkers);

// DestroyParallelContext — release a context.
void DestroyParallelContext(ParallelContext* ctx);

// LaunchParallelWorkers — start `ctx->nworkers` workers. In MyToyDB, returns
// 0 (no workers started).
int LaunchParallelWorkers(ParallelContext* ctx);

}  // namespace mytoydb::transaction

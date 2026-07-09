// parallel.h — Parallel query framework (single-process stub).
//
// Converted from PostgreSQL 15's src/backend/executor/execParallel.c.
//
// PostgreSQL launches parallel workers via fork()+shared memory to execute
// portions of a query plan concurrently. pgcpp forbids std::thread and its
// fork-based multi-process model is not wired into the executor, so this
// framework provides the API surface for parallel query infrastructure while
// always running serially (nworkers = 0). Gather/GatherMerge nodes execute
// their child plan directly in the leader process.
//
// The stubs here exist so that:
//   1. Plan nodes can declare parallel safety (ParallelQueryInfo) without
//      conditional compilation.
//   2. Future work can wire in fork-based workers behind the same API.
//   3. Parallel mode accounting (Enter/ExitParallelMode) has a single point
//      of truth that future worker launch code can hook into.
#pragma once

#include <cstdint>

namespace pgcpp::executor {

// ParallelSafety — mirrors PostgreSQL's proparallel / rel_parallel_safe.
enum class ParallelSafety : uint8_t {
    kSafe = 0,        // can run in worker or leader
    kRestricted = 1,  // can run in parallel leader only
    kUnsafe = 2,      // banned while in parallel mode
};

// ParallelContext — placeholder for a parallel query context.
//
// In PostgreSQL this holds the DSM segment, worker handles, and per-worker
// planstate copies. In pgcpp's serial stub it records the requested worker
// count (always 0 after LaunchParallelWorkers) and a flag indicating whether
// parallel mode is active.
struct ParallelContext {
    int nworkers_requested = 0;  // workers requested by the plan
    int nworkers_launched = 0;   // workers actually launched (always 0)
    bool parallel_mode = false;  // is parallel mode currently active?
};

// EnterParallelMode — mark that the backend is entering parallel mode.
// In serial stub this only flips a flag; no workers are spawned.
void EnterParallelMode();

// ExitParallelMode — mark that the backend is leaving parallel mode.
void ExitParallelMode();

// IsInParallelMode — query whether parallel mode is active.
bool IsInParallelMode();

// LaunchParallelWorkers — stub that always returns 0 (no workers launched).
// Future fork-based implementations would populate `pc` with worker handles.
int LaunchParallelWorkers(ParallelContext* pc);

// CreateParallelContext — create a context requesting `nworkers` workers.
// The returned context is owned by the caller (palloc'd in the current
// memory context).
ParallelContext* CreateParallelContext(int nworkers);

// DestroyParallelContext — release a parallel context.
void DestroyParallelContext(ParallelContext* pc);

}  // namespace pgcpp::executor

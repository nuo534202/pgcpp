// bgworker.h — Background worker framework: dynamic registration of
// custom background processes.
//
// Converted from PostgreSQL 15's src/backend/postmaster/bgworker.c.
//
// PostgreSQL allows extensions to register "background workers" — long-
// running processes forked by the postmaster that perform tasks like
// parallel query, logical replication, or custom monitoring. Each worker
// has a name, a type (auxiliary/backend/dynamic), and a main function.
//
// In pgcpp (single-process), background workers are stateful records:
// RegisterBackgroundWorker adds an entry to a registry; LaunchBackgroundWorker
// marks it running and invokes its main function (synchronously, since
// there's no real fork). The API preserves the registry/launch/dispatch
// structure for architectural fidelity.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pgcpp::server {

// BgWorkerState — lifecycle state of a background worker.
enum class BgWorkerState {
    // kRegistered — worker is registered but not yet started.
    kRegistered,
    // kStarting — LaunchBackgroundWorker called; worker is starting.
    kStarting,
    // kRunning — worker's main function is currently executing.
    kRunning,
    // kStopped — worker has finished (either completed or was terminated).
    kStopped,
};

// BgWorkerType — class of background worker.
// Matches PostgreSQL's BGWORKER_CLASS_* constants.
enum class BgWorkerType : uint8_t {
    // kAuxProcess — auxiliary process (bgwriter, checkpointer, etc.).
    kAuxProcess = 0,
    // kBackend — backend-equivalent (has full transaction state).
    kBackend,
    // kDynamic — dynamically registered (e.g. by an extension).
    kDynamic,
};

// BackgroundWorker — describes a registered background worker.
struct BackgroundWorker {
    std::string name;  // human-readable name
    BgWorkerType type = BgWorkerType::kDynamic;
    std::function<void()> main_fn;  // entry point (called when launched)
    BgWorkerState state = BgWorkerState::kRegistered;
    pid_t pid = -1;             // OS pid (only valid when running)
    int64_t start_time_ms = 0;  // timestamp when launched
    int64_t stop_time_ms = 0;   // timestamp when stopped
    int exit_code = 0;          // exit code (only valid when stopped)
};

// InitializeBgWorker — set up the worker registry (clear it).
void InitializeBgWorker();

// ResetBgWorker — clear the worker registry (for testing).
void ResetBgWorker();

// RegisterBackgroundWorker — add a worker to the registry.
// Returns the worker ID (>= 0), or -1 on error (e.g. duplicate name).
int RegisterBackgroundWorker(const BackgroundWorker& worker);

// LookupBgworkerName — find a registered worker by name.
// Returns the worker ID, or -1 if not found.
int LookupBgworkerName(const std::string& name);

// GetRegisteredBgworkers — return all registered workers.
std::vector<BackgroundWorker> GetRegisteredBgworkers();

// GetBackgroundWorker — return a copy of the worker with the given ID.
// Returns true if found, false if the ID is invalid.
bool GetBackgroundWorker(int worker_id, BackgroundWorker* out);

// LaunchBackgroundWorker — start a registered worker.
// Marks the worker as running and invokes its main function.
// Returns true on success, false if the worker is not in kRegistered state.
bool LaunchBackgroundWorker(int worker_id);

// TerminateBackgroundWorker — stop a running worker.
// Marks the worker as stopped. (In pgcpp, since workers run synchronously,
// this only has an effect on workers that have not yet been launched.)
// Returns true on success, false if the worker is not running.
bool TerminateBackgroundWorker(int worker_id);

// BgWorkerMain — dispatch to a background worker's main function.
// This is the entry point that would be called in a forked child in PG;
// in pgcpp it just invokes the registered main function.
// Returns 0 on success, non-zero on error.
int BgWorkerMain(int worker_id);

// GetBgWorkerState — return the state of a worker.
BgWorkerState GetBgWorkerState(int worker_id);

}  // namespace pgcpp::server

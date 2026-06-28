// worker.h — Logical replication apply worker.
//
// Converted from PostgreSQL 15's src/backend/replication/logical/worker.c
// (the per-subscription apply worker) and launcher.c (the worker pool).
//
// Each active logical subscription runs one apply worker (optionally
// assisted by parallel apply workers for large transactions). pgcpp
// keeps a small in-process pool (a std::vector<LogicalRepWorker>) and
// runs workers synchronously in ApplyWorkerMain (no fork).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pgcpp/transaction/xlog.hpp"

namespace pgcpp::replication {

// LogicalRepWorkerType — main apply vs. parallel apply worker.
enum class LogicalRepWorkerType : uint8_t {
    kApply = 0,     // primary apply worker for a subscription
    kParallel = 1,  // parallel apply helper for in-progress large xacts
};

// LogicalRepWorker — one worker (PG: LogicalRepWorker struct).
struct LogicalRepWorker {
    int32_t subid = 0;  // subscription OID
    int32_t relid = 0;  // relation OID (parallel workers only)
    int32_t pid = 0;    // process id (synthetic in pgcpp)
    LogicalRepWorkerType type = LogicalRepWorkerType::kApply;
    // LSNs the worker has reached.
    transaction::XLogRecPtr relstate_lsn = 0;
    transaction::XLogRecPtr commit_lsn = 0;
    bool in_use = false;            // slot in pool is occupied
    bool running = false;           // worker is currently executing
    std::string subscription_name;  // for diagnostics
};

// LogicalRepWorkerPool — the global pool (file-static singleton).
struct LogicalRepWorkerPool {
    std::vector<LogicalRepWorker> workers;
    int max_workers = 4;  // PG's max_sync_workers_per_subscription
};

// LogicalRepWorkerInit — initialize the pool.
void LogicalRepWorkerInit();

// LogicalRepWorkerReset — clear pool (tests).
void LogicalRepWorkerReset();

// LogicalRepWorkerAdd — register a worker in the pool. Returns the worker
// index, or -1 if the pool is full.
int LogicalRepWorkerAdd(int32_t subid, int32_t relid, LogicalRepWorkerType type,
                        std::string subscription_name);

// LogicalRepWorkerRemove — remove a worker by index. Returns false if the
// index is invalid.
bool LogicalRepWorkerRemove(int idx);

// LogicalRepWorkerFindBySub — return the index of the (apply) worker for a
// subscription, or -1 if none.
int LogicalRepWorkerFindBySub(int32_t subid);

// LogicalRepWorkerGetByIndex — pointer to a worker (or nullptr).
LogicalRepWorker* LogicalRepWorkerGetByIndex(int idx);

// LogicalRepWorkerCount — number of currently-registered workers.
int LogicalRepWorkerCount();

// ApplyWorkerMain — execute one iteration of the apply loop for the
// worker at `idx`. Returns 0 on success, non-zero on error.
// (Stubbed: just marks the worker as running, then not-running.)
int ApplyWorkerMain(int idx);

// ApplyWorkerWakeup — mark the worker as needing another iteration.
void ApplyWorkerWakeup(int idx);

// GetLogicalRepWorkerPool — pointer to the global pool.
LogicalRepWorkerPool* GetLogicalRepWorkerPool();

}  // namespace pgcpp::replication

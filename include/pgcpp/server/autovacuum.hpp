// autovacuum.h — Automatic VACUUM launcher and worker.
//
// Converted from PostgreSQL 15's src/backend/postmaster/autovacuum.c.
//
// PostgreSQL runs autovacuum as a launcher process that periodically
// decides which tables need VACUUM/ANALYZE, then forks worker processes
// to do the actual work. This prevents heap bloat from accumulating
// (dead tuples not being reclaimed) and keeps planner statistics fresh.
//
// In MyToyDB (single-process), autovacuum is a stateful API: the launcher
// maintains a queue of pending work items, AutoVacuumLauncherMain processes
// them, and AutoVacuumWorkerMain executes a single item. No actual fork
// is needed for the simplified model.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mytoydb::server {

// AutoVacuumWorkItem — a table scheduled for autovacuum.
struct AutoVacuumWorkItem {
    std::string database;         // database name
    std::string table;            // table name (qualified)
    bool is_analyze = false;      // true=ANALYZE only, false=VACUUM (+ANALYZE)
    bool is_vacuum = false;       // true=VACUUM
    int64_t scheduled_at_ms = 0;  // timestamp when queued
};

// AutoVacuumStats — statistics tracked by the autovacuum launcher.
struct AutoVacuumStats {
    // Number of workers launched.
    uint64_t workers_launched = 0;
    // Number of workers completed (success or fail).
    uint64_t workers_completed = 0;
    // Number of VACUUM operations run.
    uint64_t vacuums_run = 0;
    // Number of ANALYZE operations run.
    uint64_t analyzes_run = 0;
    // Number of work items skipped (already running).
    uint64_t skipped = 0;
    // Timestamp (ms since epoch) of the last worker launch.
    int64_t last_run_time_ms = 0;
    // Whether the launcher is currently running its main loop.
    bool running = false;
};

// InitializeAutoVacuum — set up autovacuum state (clear queue and stats).
void InitializeAutoVacuum();

// ResetAutoVacuum — clear autovacuum state, queue, and statistics (for testing).
void ResetAutoVacuum();

// RegisterAutoVacuumWorkItem — queue a table for autovacuum.
// Returns true if the item was added, false if it was already in the queue.
bool RegisterAutoVacuumWorkItem(const AutoVacuumWorkItem& item);

// AutoVacuumLauncherMain — main loop of the launcher (simplified: processes
// pending work items up to `max_workers` of them). Returns the number of
// workers launched.
int AutoVacuumLauncherMain(int max_workers);

// AutoVacuumWorkerMain — main loop of a worker process (simplified: executes
// a single work item). Returns 0 on success, non-zero on error.
int AutoVacuumWorkerMain(const AutoVacuumWorkItem& item);

// AutoVacuumShutdown — request the launcher to stop (sets running=false).
void AutoVacuumShutdown();

// AutoVacuumIsRunning — true if the launcher is in its main loop.
bool AutoVacuumIsRunning();

// GetPendingAutoVacuumItems — return a copy of the pending work queue.
std::vector<AutoVacuumWorkItem> GetPendingAutoVacuumItems();

// GetAutoVacuumStats — return the current autovacuum statistics.
AutoVacuumStats GetAutoVacuumStats();

}  // namespace mytoydb::server

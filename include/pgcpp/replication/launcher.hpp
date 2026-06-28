// launcher.h — Logical replication launcher.
//
// Converted from PostgreSQL 15's src/backend/replication/logical/launcher.c.
//
// The launcher is a single background process that periodically scans
// pg_subscription and starts / stops apply workers as needed. MyToyDB
// keeps a tiny state object (running flag, last-run timestamp, next-id)
// and exposes ApplyLauncherMain / ApplyLauncherWakeup as stubs.
#pragma once

#include <cstdint>

namespace mytoydb::replication {

// LogicalRepLauncherState — the launcher's state (file-static singleton).
struct LogicalRepLauncherState {
    bool running = false;
    int64_t last_run_time_ms = 0;      // ms since epoch
    int64_t next_subscription_id = 1;  // monotonic id generator
    int workers_started = 0;
    int workers_stopped = 0;
    bool shutdown_requested = false;
};

// ApplyLauncherInit — initialize launcher state.
void ApplyLauncherInit();

// ApplyLauncherReset — clear state (tests).
void ApplyLauncherReset();

// ApplyLauncherMain — run the launcher's main loop for up to
// `max_iterations` iterations. Returns the number of workers started.
// (Stub: just records the cycle count and returns 0.)
int ApplyLauncherMain(int max_iterations);

// ApplyLauncherWakeup — request the launcher to scan subscriptions again.
void ApplyLauncherWakeup();

// ApplyLauncherShutdown — request the launcher to stop.
void ApplyLauncherShutdown();

// ApplyLauncherIsRunning — true if the launcher is in its main loop.
bool ApplyLauncherIsRunning();

// AllocateNextSubscriptionId — return the next monotonic subscription id.
int64_t AllocateNextSubscriptionId();

// GetLogicalRepLauncherState — pointer to the global launcher state.
LogicalRepLauncherState* GetLogicalRepLauncherState();

}  // namespace mytoydb::replication

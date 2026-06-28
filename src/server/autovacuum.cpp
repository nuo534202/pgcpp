// autovacuum.cpp — Automatic VACUUM launcher and worker.
//
// Converted from PostgreSQL 15's src/backend/postmaster/autovacuum.c.
//
// The launcher maintains a queue of tables that need VACUUM or ANALYZE.
// AutoVacuumLauncherMain processes the queue by calling
// AutoVacuumWorkerMain for each item. In MyToyDB (single-process), the
// worker executes synchronously rather than being forked.
#include "pgcpp/server/autovacuum.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "pgcpp/server/interrupt.hpp"

namespace mytoydb::server {

namespace {

std::vector<AutoVacuumWorkItem>& WorkQueue() {
    static std::vector<AutoVacuumWorkItem> q;
    return q;
}

AutoVacuumStats& Stats() {
    static AutoVacuumStats s;
    return s;
}

bool& Running() {
    static bool r = false;
    return r;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

void InitializeAutoVacuum() {
    WorkQueue().clear();
    Stats() = AutoVacuumStats{};
    Running() = false;
}

void ResetAutoVacuum() {
    InitializeAutoVacuum();
}

bool RegisterAutoVacuumWorkItem(const AutoVacuumWorkItem& item) {
    // Skip if the same (database, table) pair is already queued.
    for (const auto& existing : WorkQueue()) {
        if (existing.database == item.database && existing.table == item.table) {
            return false;
        }
    }
    AutoVacuumWorkItem copy = item;
    if (copy.scheduled_at_ms == 0) {
        copy.scheduled_at_ms = NowMs();
    }
    WorkQueue().push_back(std::move(copy));
    return true;
}

int AutoVacuumLauncherMain(int max_workers) {
    Running() = true;
    auto& s = Stats();
    int launched = 0;

    for (int i = 0; i < max_workers && !WorkQueue().empty(); ++i) {
        if (InterruptFlags::ShutdownRequested.load()) {
            break;
        }

        AutoVacuumWorkItem item = std::move(WorkQueue().front());
        WorkQueue().erase(WorkQueue().begin());

        ++s.workers_launched;
        s.last_run_time_ms = NowMs();

        int rc = AutoVacuumWorkerMain(item);
        ++s.workers_completed;

        if (rc == 0) {
            if (item.is_vacuum) {
                ++s.vacuums_run;
            }
            if (item.is_analyze) {
                ++s.analyzes_run;
            }
        }
        ++launched;
    }

    Running() = false;
    return launched;
}

int AutoVacuumWorkerMain(const AutoVacuumWorkItem& item) {
    // MyToyDB simplification: VACUUM/ANALYZE are no-ops that return success
    // (the underlying commands module already handles them eagerly). The
    // worker's role is to record the work item execution in stats.
    // Returns 0 on success, non-zero on error.
    if (item.database.empty() || item.table.empty()) {
        return 1;
    }
    return 0;
}

void AutoVacuumShutdown() {
    InterruptFlags::ShutdownRequested = true;
}

bool AutoVacuumIsRunning() {
    return Running();
}

std::vector<AutoVacuumWorkItem> GetPendingAutoVacuumItems() {
    return WorkQueue();
}

AutoVacuumStats GetAutoVacuumStats() {
    return Stats();
}

}  // namespace mytoydb::server

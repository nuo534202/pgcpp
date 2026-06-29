// launcher.cpp — Logical replication launcher.
//
// Converted from PostgreSQL 15's src/backend/replication/logical/launcher.c.
// Stubbed: ApplyLauncherMain just records the cycle count and returns 0.
#include "replication/launcher.hpp"

#include <chrono>
#include <cstdint>

namespace pgcpp::replication {

namespace {

LogicalRepLauncherState& State() {
    static LogicalRepLauncherState s;
    return s;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

void ApplyLauncherInit() {
    State() = LogicalRepLauncherState{};
}

void ApplyLauncherReset() {
    ApplyLauncherInit();
}

int ApplyLauncherMain(int max_iterations) {
    State().running = true;
    int started = 0;
    for (int i = 0; i < max_iterations; ++i) {
        if (State().shutdown_requested) {
            break;
        }
        State().last_run_time_ms = NowMs();
        // Stubbed: no actual subscription scan happens. We just count one
        // cycle. (A real launcher would walk pg_subscription and start/stop
        // apply workers via the worker pool.)
    }
    State().workers_started = started;
    State().running = false;
    return started;
}

void ApplyLauncherWakeup() {
    // No-op in the stub: just refresh last_run_time_ms so callers can
    // observe that the wakeup was received.
    State().last_run_time_ms = NowMs();
}

void ApplyLauncherShutdown() {
    State().shutdown_requested = true;
}

bool ApplyLauncherIsRunning() {
    return State().running;
}

int64_t AllocateNextSubscriptionId() {
    return State().next_subscription_id++;
}

LogicalRepLauncherState* GetLogicalRepLauncherState() {
    return &State();
}

}  // namespace pgcpp::replication

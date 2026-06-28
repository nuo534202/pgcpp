// bgworker.cpp — Background worker framework: registry of background workers.
//
// Converted from PostgreSQL 15's src/backend/postmaster/bgworker.c.
//
// In PG, the postmaster maintains a registry of background workers and
// forks them on startup or on demand. Each worker has a name, type, and
// main function. MyToyDB preserves the registry/launch/dispatch structure
// but runs workers synchronously (no actual fork).
#include "mytoydb/server/bgworker.hpp"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mytoydb::server {

namespace {

std::vector<BackgroundWorker>& Registry() {
    static std::vector<BackgroundWorker> r;
    return r;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

void InitializeBgWorker() {
    Registry().clear();
}

void ResetBgWorker() {
    InitializeBgWorker();
}

int RegisterBackgroundWorker(const BackgroundWorker& worker) {
    // Reject duplicate names.
    for (const auto& w : Registry()) {
        if (w.name == worker.name) {
            return -1;
        }
    }
    BackgroundWorker w = worker;
    w.state = BgWorkerState::kRegistered;
    w.pid = -1;
    w.start_time_ms = 0;
    w.stop_time_ms = 0;
    w.exit_code = 0;
    Registry().push_back(std::move(w));
    return static_cast<int>(Registry().size() - 1);
}

int LookupBgworkerName(const std::string& name) {
    for (std::size_t i = 0; i < Registry().size(); ++i) {
        if (Registry()[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::vector<BackgroundWorker> GetRegisteredBgworkers() {
    return Registry();
}

bool GetBackgroundWorker(int worker_id, BackgroundWorker* out) {
    if (worker_id < 0 || static_cast<std::size_t>(worker_id) >= Registry().size()) {
        return false;
    }
    if (out != nullptr) {
        *out = Registry()[static_cast<std::size_t>(worker_id)];
    }
    return true;
}

bool LaunchBackgroundWorker(int worker_id) {
    if (worker_id < 0 || static_cast<std::size_t>(worker_id) >= Registry().size()) {
        return false;
    }
    auto& w = Registry()[static_cast<std::size_t>(worker_id)];
    if (w.state != BgWorkerState::kRegistered) {
        return false;
    }
    w.state = BgWorkerState::kStarting;
    w.start_time_ms = NowMs();
    w.pid = getpid();
    w.state = BgWorkerState::kRunning;

    // Invoke the main function synchronously.
    if (w.main_fn) {
        w.main_fn();
    }

    w.state = BgWorkerState::kStopped;
    w.stop_time_ms = NowMs();
    w.exit_code = 0;
    return true;
}

bool TerminateBackgroundWorker(int worker_id) {
    if (worker_id < 0 || static_cast<std::size_t>(worker_id) >= Registry().size()) {
        return false;
    }
    auto& w = Registry()[static_cast<std::size_t>(worker_id)];
    if (w.state != BgWorkerState::kRunning && w.state != BgWorkerState::kRegistered) {
        return false;
    }
    w.state = BgWorkerState::kStopped;
    w.stop_time_ms = NowMs();
    return true;
}

int BgWorkerMain(int worker_id) {
    if (worker_id < 0 || static_cast<std::size_t>(worker_id) >= Registry().size()) {
        return -1;
    }
    auto& w = Registry()[static_cast<std::size_t>(worker_id)];
    if (w.state != BgWorkerState::kRegistered) {
        return -1;
    }
    if (!LaunchBackgroundWorker(worker_id)) {
        return -1;
    }
    return 0;
}

BgWorkerState GetBgWorkerState(int worker_id) {
    if (worker_id < 0 || static_cast<std::size_t>(worker_id) >= Registry().size()) {
        return BgWorkerState::kStopped;
    }
    return Registry()[static_cast<std::size_t>(worker_id)].state;
}

}  // namespace mytoydb::server

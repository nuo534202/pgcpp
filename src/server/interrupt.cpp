// interrupt.cpp — Signal/interrupt handling for backend and auxiliary processes.
//
// Converted from PostgreSQL 15's src/backend/tcop/postgres.c (interrupt section)
// and src/backend/storage/ipc/signalfuncs.c.
//
// Implements the deferred-interrupt model: signal handlers only set atomic
// flags; the main loop calls HandleInterrupts() at safe points to dispatch
// pending interrupts to registered handlers. C++ exceptions are disabled,
// so query-cancel sets a flag that the executor polls via InterruptRequested().
#include "pgcpp/server/interrupt.hpp"

#include <chrono>
#include <csignal>
#include <cstring>
#include <map>
#include <thread>
#include <utility>
#include <vector>

namespace pgcpp::server {

// --- Static atomic flag definitions ---
std::atomic<bool> InterruptFlags::InterruptPending{false};
std::atomic<bool> InterruptFlags::QueryCancelPending{false};
std::atomic<bool> InterruptFlags::ProcDiePending{false};
std::atomic<bool> InterruptFlags::CheckpointTimeout{false};
std::atomic<bool> InterruptFlags::ReloadConfigPending{false};
std::atomic<bool> InterruptFlags::ShutdownRequested{false};
std::atomic<bool> InterruptFlags::BgWriterShutdownRequested{false};
std::atomic<bool> InterruptFlags::WalWriterShutdownRequested{false};

void ResetInterruptFlags() {
    InterruptFlags::InterruptPending = false;
    InterruptFlags::QueryCancelPending = false;
    InterruptFlags::ProcDiePending = false;
    InterruptFlags::CheckpointTimeout = false;
    InterruptFlags::ReloadConfigPending = false;
    InterruptFlags::ShutdownRequested = false;
    InterruptFlags::BgWriterShutdownRequested = false;
    InterruptFlags::WalWriterShutdownRequested = false;
}

// --- Signal handlers (signal-safe) ---

void HandleQueryCancelSignal(int /*sig*/) {
    InterruptFlags::QueryCancelPending = true;
    InterruptFlags::InterruptPending = true;
}

void HandleProcDieSignal(int /*sig*/) {
    InterruptFlags::ProcDiePending = true;
    InterruptFlags::InterruptPending = true;
}

void HandleReloadConfigSignal(int /*sig*/) {
    InterruptFlags::ReloadConfigPending = true;
    InterruptFlags::InterruptPending = true;
}

void HandleShutdownSignal(int /*sig*/) {
    InterruptFlags::ShutdownRequested = true;
    InterruptFlags::InterruptPending = true;
}

void HandleChildShutdownSignal(int /*sig*/) {
    InterruptFlags::ShutdownRequested = true;
    InterruptFlags::ProcDiePending = true;
    InterruptFlags::InterruptPending = true;
}

// --- Signal handler installation ---

void InstallBackendSignalHandlers() {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    sa.sa_handler = HandleQueryCancelSignal;
    sigaction(SIGINT, &sa, nullptr);

    sa.sa_handler = HandleProcDieSignal;
    sigaction(SIGTERM, &sa, nullptr);

    sa.sa_handler = HandleReloadConfigSignal;
    sigaction(SIGHUP, &sa, nullptr);

    // Ignore SIGPIPE — write errors are handled by return values.
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, nullptr);
}

void ResetBackendSignalHandlers() {
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
}

void InstallAuxProcessSignalHandlers() {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    sa.sa_handler = HandleShutdownSignal;
    sigaction(SIGTERM, &sa, nullptr);

    sa.sa_handler = HandleReloadConfigSignal;
    sigaction(SIGHUP, &sa, nullptr);

    // Ignore SIGPIPE.
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, nullptr);
}

// --- Named interrupt handler registry ---

namespace {

struct NamedHandler {
    int id;
    std::string name;
    InterruptHandlerFn fn;
};

std::vector<NamedHandler>& Handlers() {
    static std::vector<NamedHandler> h;
    return h;
}

int& NextHandlerId() {
    static int id = 1;
    return id;
}

}  // namespace

int RegisterInterruptHandler(const std::string& name, InterruptHandlerFn handler) {
    int id = NextHandlerId()++;
    Handlers().push_back({id, name, std::move(handler)});
    return id;
}

void UnregisterInterruptHandler(int handler_id) {
    auto& h = Handlers();
    for (auto it = h.begin(); it != h.end(); ++it) {
        if (it->id == handler_id) {
            h.erase(it);
            return;
        }
    }
}

void ClearInterruptHandlers() {
    Handlers().clear();
}

void SignalInterruptHandler(const std::string& name) {
    for (auto& entry : Handlers()) {
        if (entry.name == name && entry.fn) {
            entry.fn();
        }
    }
}

// --- Deferred interrupt processing ---

void HandleInterrupts() {
    if (InterruptFlags::QueryCancelPending.exchange(false)) {
        SignalInterruptHandler("QueryCancel");
    }
    if (InterruptFlags::ProcDiePending.exchange(false)) {
        SignalInterruptHandler("ProcDie");
    }
    if (InterruptFlags::CheckpointTimeout.exchange(false)) {
        SignalInterruptHandler("CheckpointTimeout");
    }
    if (InterruptFlags::ReloadConfigPending.exchange(false)) {
        SignalInterruptHandler("ReloadConfig");
    }
    if (InterruptFlags::ShutdownRequested.exchange(false)) {
        SignalInterruptHandler("Shutdown");
    }
    if (InterruptFlags::BgWriterShutdownRequested.exchange(false)) {
        SignalInterruptHandler("BgWriterShutdown");
    }
    if (InterruptFlags::WalWriterShutdownRequested.exchange(false)) {
        SignalInterruptHandler("WalWriterShutdown");
    }
    InterruptFlags::InterruptPending = false;
}

void CheckForInterrupts() {
    if (InterruptFlags::InterruptPending.load()) {
        HandleInterrupts();
    }
}

bool InterruptRequested() {
    return InterruptFlags::QueryCancelPending.load() || InterruptFlags::ProcDiePending.load();
}

bool WaitForInterrupt(int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (InterruptFlags::InterruptPending.load()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return InterruptFlags::InterruptPending.load();
}

}  // namespace pgcpp::server

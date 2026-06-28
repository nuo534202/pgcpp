// pqsignal.cpp — Signal-handling utilities.
//
// Wraps sigaction(2) with PG's pqsignal() API. SA_RESTART is set on all
// handlers (matching PG's behaviour for non-blocking sockets). The
// `no_restart` variant is used for signals that must interrupt blocking
// syscalls (SIGALRM, SIGINT).
#include "pgcpp/protocol/pqsignal.hpp"

#include <csignal>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace pgcpp::protocol {

namespace {

std::mutex& HandlerMutex() {
    static std::mutex m;
    return m;
}

// Records the handler currently installed by pqsignal (for test inspection).
std::unordered_map<int, SignalHandler>& InstalledHandlers() {
    static std::unordered_map<int, SignalHandler> m;
    return m;
}

void RecordHandler(int signo, SignalHandler h) {
    std::lock_guard<std::mutex> g(HandlerMutex());
    InstalledHandlers()[signo] = h;
}

}  // namespace

SignalHandler pqsignal(int signo, SignalHandler handler) {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    struct sigaction old;
    if (sigaction(signo, &sa, &old) != 0) {
        return SIG_ERR;
    }
    RecordHandler(signo, handler);
    return old.sa_handler;
}

SignalHandler pqsignal_no_restart(int signo, SignalHandler handler) {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    struct sigaction old;
    if (sigaction(signo, &sa, &old) != 0) {
        return SIG_ERR;
    }
    RecordHandler(signo, handler);
    return old.sa_handler;
}

int pq_block_sigalrm() {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    return pthread_sigmask(SIG_BLOCK, &set, nullptr);
}

int pq_reset_sigalrm() {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    return pthread_sigmask(SIG_UNBLOCK, &set, nullptr);
}

int pq_sigprocmask(int how, const sigset_t* set, sigset_t* oldset) {
    return pthread_sigmask(how, set, oldset);
}

SignalHandler GetInstalledHandler(int signo) {
    std::lock_guard<std::mutex> g(HandlerMutex());
    auto it = InstalledHandlers().find(signo);
    if (it == InstalledHandlers().end())
        return nullptr;
    return it->second;
}

void ResetAllSignalHandlers() {
    std::lock_guard<std::mutex> g(HandlerMutex());
    for (auto& kv : InstalledHandlers()) {
        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_DFL;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(kv.first, &sa, nullptr);
    }
    InstalledHandlers().clear();
}

}  // namespace pgcpp::protocol

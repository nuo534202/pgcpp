// latch.cpp — Latch and WaitEventSet primitives.
//
// Converted from PostgreSQL 15's src/backend/storage/ipc/latch.c.
#include "pgcpp/storage/ipc/latch.hpp"

#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>  // NOLINT
#include <cstring>
#include <thread>  // NOLINT

namespace pgcpp::storage {

void WaitEventSet::AddLatch(Latch* latch) {
    WaitEvent ev;
    ev.events = kWaitLatchSet;
    ev.fd = -1;
    ev.latch = latch;
    events_.push_back(ev);
}

void WaitEventSet::AddSocket(int fd, uint32_t events) {
    WaitEvent ev;
    ev.events = events;
    ev.fd = fd;
    ev.latch = nullptr;
    events_.push_back(ev);
}

void InitLatch(Latch* latch) {
    latch->is_set = false;
}

bool SetLatch(Latch* latch) {
    bool was_set = latch->is_set;
    latch->is_set = true;
    return !was_set;
}

void ResetLatch(Latch* latch) {
    latch->is_set = false;
}

bool TestLatch(const Latch* latch) {
    return latch->is_set;
}

uint32_t WaitLatch(Latch* latch, int64_t timeout_ms, uint32_t /*wait_event_mask*/) {
    // Fast path: already set.
    if (latch->is_set) {
        return kWaitLatchSet;
    }
    // Poll: sleep 1ms between checks until set or timeout.
    auto start = std::chrono::steady_clock::now();
    while (!latch->is_set) {
        if (timeout_ms >= 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start)
                               .count();
            if (elapsed >= timeout_ms) {
                return kWaitTimeout;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return kWaitLatchSet;
}

int WaitEventSetWait(WaitEventSet* set, int64_t timeout_ms,
                     std::vector<WaitEvent>* occurred_events) {
    occurred_events->clear();
    if (set == nullptr || set->NumEvents() == 0) {
        return 0;
    }
    // Build pollfd array for socket events.
    std::vector<pollfd> pfds;
    for (const auto& ev : set->Events()) {
        if (ev.fd >= 0) {
            pollfd pfd{};
            pfd.fd = ev.fd;
            pfd.events = 0;
            if (ev.events & kWaitSocketReadable) {
                pfd.events |= POLLIN;
            }
            if (ev.events & kWaitSocketWriteable) {
                pfd.events |= POLLOUT;
            }
            pfds.push_back(pfd);
        }
    }

    auto start = std::chrono::steady_clock::now();
    while (true) {
        // First check latch events (immediate).
        for (const auto& ev : set->Events()) {
            if ((ev.events & kWaitLatchSet) && ev.latch != nullptr && ev.latch->is_set) {
                occurred_events->push_back(ev);
            }
        }
        if (!occurred_events->empty()) {
            return static_cast<int>(occurred_events->size());
        }
        // Then poll sockets (with a 1ms timeout per iteration so we can re-check latches).
        if (!pfds.empty()) {
            int pr = ::poll(pfds.data(), pfds.size(), 1);
            if (pr < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return -1;
            }
            if (pr > 0) {
                int idx = 0;
                for (const auto& ev : set->Events()) {
                    if (ev.fd >= 0) {
                        const auto& pfd = pfds[idx];
                        if (pfd.revents != 0) {
                            occurred_events->push_back(ev);
                        }
                    }
                    ++idx;
                }
                if (!occurred_events->empty()) {
                    return static_cast<int>(occurred_events->size());
                }
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // Check timeout.
        if (timeout_ms >= 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start)
                               .count();
            if (elapsed >= timeout_ms) {
                return static_cast<int>(occurred_events->size());
            }
        }
    }
}

}  // namespace pgcpp::storage

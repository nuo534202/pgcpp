// latch.h — Latch and WaitEventSet primitives.
//
// Converted from PostgreSQL 15's src/include/storage/latch.h and
// src/backend/storage/ipc/latch.c.
//
// A Latch is a single-waiter primitive that can be Set, Reset, and Tested.
// Waiters block in WaitLatch() until the latch is set (or a timeout/notify
// occurs). A WaitEventSet allows waiting on multiple latches and/or sockets
// simultaneously via poll(2).
//
// In PostgreSQL, latches use shared-memory state and self-pipes for signal
// wakeups. MyToyDB is single-process, so the state lives in heap-allocated
// structs and waits are implemented via poll() with millisecond precision.
#pragma once

#include <cstdint>
#include <vector>

namespace mytoydb::storage {

// WaitEventSet event mask bits (mirrors WL_* in PostgreSQL).
constexpr uint32_t kWaitLatchSet = 0x0001;           // WL_LATCH_SET
constexpr uint32_t kWaitSocketReadable = 0x0002;     // WL_SOCKET_READABLE
constexpr uint32_t kWaitSocketWriteable = 0x0004;    // WL_SOCKET_WRITEABLE
constexpr uint32_t kWaitTimeout = 0x0008;            // WL_TIMEOUT
constexpr uint32_t kWaitExitOnSignalDeath = 0x0010;  // WL_EXIT_ON_PM_DEATH

// Latch — a single-waiter primitive.
//
// Matches PG's volatile sig_atomic_t is_set + signal handling fields.
// In MyToyDB is_set is a plain bool (no signal wakeups needed).
struct Latch {
    bool is_set = false;
};

// WaitEvent — describes one event registered in a WaitEventSet.
struct WaitEvent {
    uint32_t events = 0;     // mask of kWait* bits
    int fd = -1;             // socket fd (or -1 for latch-only)
    Latch* latch = nullptr;  // latch pointer (or nullptr for socket-only)
    void* user_data = nullptr;
};

// WaitEventSet — a collection of latch/socket events to wait on.
//
// Implemented as a std::vector<WaitEvent> wrapper. PG stores it in shared
// memory; MyToyDB keeps it in a heap allocation owned by the caller.
class WaitEventSet {
public:
    WaitEventSet() = default;

    // Add a latch event (kWaitLatchSet).
    void AddLatch(Latch* latch);
    // Add a socket event with the given mask (kWaitSocketReadable / kWaitSocketWriteable).
    void AddSocket(int fd, uint32_t events);
    // Number of registered events.
    std::size_t NumEvents() const { return events_.size(); }
    // Read-only access to the events.
    const std::vector<WaitEvent>& Events() const { return events_; }
    // Clear all events.
    void Clear() { events_.clear(); }

private:
    std::vector<WaitEvent> events_;
};

// InitLatch — initialize a Latch to the unset state.
void InitLatch(Latch* latch);

// SetLatch — set the latch and wake any waiter.
// Returns true if the latch transitioned from unset to set.
bool SetLatch(Latch* latch);

// ResetLatch — clear the latch's set state.
// Caller must own the latch (be the only waiter).
void ResetLatch(Latch* latch);

// TestLatch — return whether the latch is currently set.
bool TestLatch(const Latch* latch);

// WaitLatch — block until the latch is set, a signal arrives, or timeout.
// Returns a bitmask of kWaitLatchSet / kWaitTimeout.
// timeout_ms < 0 means infinite wait; timeout_ms == 0 means no wait.
uint32_t WaitLatch(Latch* latch, int64_t timeout_ms, uint32_t wait_event_mask);

// WaitEventSetWait — block until any event in the set fires or timeout.
// Returns the number of events that fired (filled into occurred_events).
// timeout_ms < 0 means infinite wait; timeout_ms == 0 means poll-once.
int WaitEventSetWait(WaitEventSet* set, int64_t timeout_ms,
                     std::vector<WaitEvent>* occurred_events);

}  // namespace mytoydb::storage

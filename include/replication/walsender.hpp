// walsender.h — Walsender (streaming-replication server side).
//
// Converted from PostgreSQL 15's src/backend/replication/walsender.c.
//
// A walsender is a backend whose role is to feed WAL records to a
// connected standby (or to a base-backup client). Each walsender keeps
// track of four LSNs reported back by the standby:
//   - sent_ptr:   the LSN we (the server) have sent up to.
//   - write_ptr:  the LSN the standby has written to disk.
//   - flush_ptr:  the LSN the standby has fsync'd.
//   - apply_ptr:  the LSN the standby has replayed (or InvalidXLogRecPtr).
//
// In PostgreSQL, walsender state lives in shared memory (WalSndCtl) so
// that other backends can query it for sync-rep waits. pgcpp is
// single-process, so we hold an in-memory std::vector<WalSnd> in a
// file-static singleton. The API surface mirrors PG: WalSndInit,
// WalSndGetState, WalSndSetState, WalSndWakeup, WalSndWaitForWal.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "replication/replutil.hpp"
#include "transaction/xlog.hpp"

namespace pgcpp::replication {

// Maximum number of walsenders (mirrors PG's max_wal_senders default of 10).
constexpr int kMaxWalSenders = 10;

// WalSndState — lifecycle state of a single walsender.
enum class WalSndState : uint8_t {
    kStartup = 0,    // just connected, not yet streaming
    kCatchup = 1,    // sending historic WAL until the standby catches up
    kStreaming = 2,  // streaming live WAL
    kBackup = 3,     // performing a base backup (not WAL streaming)
    kStopping = 4,   // shutting down
};

// WalSnd — per-sender state. Matches PostgreSQL's WalSnd struct.
// (PG's `pid` is int32; we use int32_t to mirror that.)
struct WalSnd {
    int32_t pid = 0;
    WalSndState state = WalSndState::kStartup;
    transaction::XLogRecPtr sent_ptr = 0;
    transaction::XLogRecPtr write_ptr = 0;
    transaction::XLogRecPtr flush_ptr = 0;
    transaction::XLogRecPtr apply_ptr = 0;
    std::string slot_name;              // logical/physical slot, may be empty
    std::string application_name;       // standby's application_name
    int64_t sync_standby_priority = 0;  // 0 = not a sync standby
    bool need_to_flush = false;         // pending reply request
};

// WalSndCtlData — the global array of walsenders (PG: WalSndCtl struct).
struct WalSndCtlData {
    std::vector<WalSnd> walsenders;
    // LSN at least one standby must catch up to before sync-rep waiters
    // can be woken. (Equivalent to PG's WalSndCtl->lsn[].)
    transaction::XLogRecPtr lsn_target = 0;
};

// WalSndInit — initialize the walsender subsystem (clear the array).
void WalSndInit();

// WalSndReset — clear all walsender state (alias for testing).
void WalSndReset();

// WalSndGetState — return the state of the i-th sender, or kStopping if i is
// out of range.
WalSndState WalSndGetState(int idx);

// WalSndSetState — change the state of the i-th sender. Returns false if
// the index is invalid.
bool WalSndSetState(int idx, WalSndState new_state);

// WalSndAlloc — register a new walsender and return its index. Returns -1
// if the array is at capacity (kMaxWalSenders).
int WalSndAlloc(int32_t pid, std::string application_name);

// WalSndGetByPid — look up a sender by its pid. Returns -1 if not found.
int WalSndGetByPid(int32_t pid);

// WalSndGetByIndex — return a pointer to the i-th sender, or nullptr.
WalSnd* WalSndGetByIndex(int idx);

// WalSndCount — number of currently registered senders.
int WalSndCount();

// WalSndWakeup — mark all senders as needing to flush on their next cycle.
void WalSndWakeup();

// WalSndWakeupWaitingForWal — wake any sender waiting for the given LSN.
void WalSndWakeupWaitingForWal(transaction::XLogRecPtr lsn);

// WalSndWaitForWal — block (or poll, in the stubbed pgcpp implementation)
// until the requested LSN has been flushed by all senders in `wait_mode`.
// Returns true if the LSN is reachable in `max_iterations` cycles.
//   wait_mode = -1 means "any sender", >= 0 means "specific sender index".
bool WalSndWaitForWal(transaction::XLogRecPtr lsn, int wait_mode, int max_iterations);

// WalSndUpdateLsn — update one of the four LSN slots for the i-th sender.
enum class WalSndLsnKind { kSent, kWrite, kFlush, kApply };
void WalSndUpdateLsn(int idx, WalSndLsnKind kind, transaction::XLogRecPtr lsn);

// WalSndRemove — remove the i-th sender (shift subsequent senders down).
// Returns true on success.
bool WalSndRemove(int idx);

// GetWalSndCtl — return a pointer to the global WalSndCtlData singleton.
WalSndCtlData* GetWalSndCtl();

}  // namespace pgcpp::replication

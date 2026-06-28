// walreceiver.h — Walreceiver (streaming-replication client side).
//
// Converted from PostgreSQL 15's src/backend/replication/walreceiver.c.
//
// The walreceiver is a process that connects to a primary, requests
// streaming from a given LSN, and writes the received WAL into local
// pg_wal storage before applying it. PG stores its state in shared
// memory (WalRcvData *WalRcv).
//
// MyToyDB keeps a single in-process WalRcvData singleton (only one
// walreceiver is ever active per node in the simplified model). The
// network code is stubbed: WalRcvStart records the requested conninfo /
// slotname / startpoint and transitions the state to kStreaming.
#pragma once

#include <cstdint>
#include <string>

#include "mytoydb/replication/replutil.hpp"
#include "mytoydb/transaction/xlog.hpp"

namespace mytoydb::replication {

// WalRcvState — lifecycle of the walreceiver. Mirrors PG's WALRCV_STOPPED
// ... WALRCV_STREAMING enum (one value dropped: PG has an intermediate
// WALRCV_WAITING that we model as part of kStreaming).
enum class WalRcvState : uint8_t {
    kStopped = 0,    // not running
    kStart = 1,      // about to start, before connect()
    kStreaming = 2,  // connected and receiving
};

// WalRcvStreamState — what the walreceiver is currently doing within
// kStreaming (the streaming sub-state machine).
enum class WalRcvStreamState : uint8_t {
    kNone = 0,
    kStreamStart,     // just started a stream (after primary handshake)
    kStreamContinue,  // ongoing stream
    kStreamStop,      // stream paused/ended
};

// WalRcvData — per-walreceiver state. Matches PG's WalRcvData.
struct WalRcvData {
    std::string conninfo;  // libpq conninfo string
    std::string slotname;  // replication slot name (may be empty)
    transaction::XLogRecPtr startpoint = 0;
    WalRcvState state = WalRcvState::kStopped;
    WalRcvStreamState stream_state = WalRcvStreamState::kNone;
    int32_t pid = 0;
    // LSNs the walreceiver has reported back to the primary:
    transaction::XLogRecPtr receive_ptr = 0;
    transaction::XLogRecPtr write_ptr = 0;
    transaction::XLogRecPtr flush_ptr = 0;
    transaction::XLogRecPtr apply_ptr = 0;
    // Last received message type (diagnostics).
    LogicalRepMsgType last_msg_type = LogicalRepMsgType::kBegin;
};

// WalRcvInit — clear the singleton to its initial state.
void WalRcvInit();

// WalRcvReset — alias used in tests.
void WalRcvReset();

// WalRcvStart — begin receiving. Returns false if a receiver is already
// active or conninfo is empty.
bool WalRcvStart(std::string conninfo, std::string slotname, transaction::XLogRecPtr startpoint);

// WalRcvStop — stop the active receiver (if any). Returns the PID of the
// stopped receiver, or 0 if none was running.
int32_t WalRcvStop();

// WalRcvGetState — return the current lifecycle state.
WalRcvState WalRcvGetState();

// WalRcvGetStreamState — return the streaming sub-state.
WalRcvStreamState WalRcvGetStreamState();

// WalRcvGetStateName — string form of a WalRcvState (for diagnostics).
const char* WalRcvStateName(WalRcvState s);

// WalRcvSetPid — assign a pid to the receiver (used by tests).
void WalRcvSetPid(int32_t pid);

// WalRcvReportLsn — advance one of the receiver's LSNs (write/flush/apply).
enum class WalRcvLsnKind { kReceive, kWrite, kFlush, kApply };
void WalRcvReportLsn(WalRcvLsnKind kind, transaction::XLogRecPtr lsn);

// WalRcvGetData — return a pointer to the global singleton (may be null).
WalRcvData* GetWalRcvData();

}  // namespace mytoydb::replication

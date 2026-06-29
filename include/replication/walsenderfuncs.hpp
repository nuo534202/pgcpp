// walsenderfuncs.h — helper functions used by walsender.c.
//
// Converted from PostgreSQL 15's src/backend/replication/walsender.c
// (the message-handling subset: WalSndWriteData, WalSndKeepalive,
// WalSndReply / ProcessStandbyReplyMessage, etc.).
//
// These functions form the public in-process API the rest of the
// replication code calls to push WAL to a connected standby and to
// exchange keepalive / reply messages with it. Network I/O is stubbed:
// each call records the LSN it "would have sent" on the targeted WalSnd
// and returns a small struct describing what was sent.
#pragma once

#include <cstdint>

#include "replication/walsender.hpp"
#include "transaction/xlog.hpp"

namespace pgcpp::replication {

// WalSndMessageResult — what a single "send" call produced. Returned by
// value because the values are tiny and the call sites want the result
// synchronously.
struct WalSndMessageResult {
    int sender_idx = -1;                    // index of the WalSnd we updated
    transaction::XLogRecPtr start_lsn = 0;  // first LSN we (logically) sent
    transaction::XLogRecPtr end_lsn = 0;    // LSN just after the last byte
    uint64_t bytes_sent = 0;                // number of WAL bytes
    bool reply_requested = false;           // whether we asked the standby to ACK
};

// WalSndWriteData — "send" a chunk of WAL [start, end) on the i-th sender.
// Updates sent_ptr on the WalSnd. Returns a description of the send.
// `reply_requested` schedules a keepalive-with-reply on the next cycle.
WalSndMessageResult WalSndWriteData(int sender_idx, transaction::XLogRecPtr start_lsn,
                                    transaction::XLogRecPtr end_lsn, bool reply_requested);

// WalSndKeepalive — send a keepalive message on the i-th sender, optionally
// requesting a reply. Updates sent_ptr to current WAL insert position so
// standby stays informed.
WalSndMessageResult WalSndKeepalive(int sender_idx, bool reply_requested);

// WalSndReply — process a reply received from the standby. Updates the
// sender's write/flush/apply LSNs from the reply's contents.
struct WalSndReplyMessage {
    transaction::XLogRecPtr write_lsn = 0;
    transaction::XLogRecPtr flush_lsn = 0;
    transaction::XLogRecPtr apply_lsn = 0;
    int64_t send_time_ms = 0;
    bool reply_requested = false;  // standby asked for another reply
};

// ApplyStandbyReply — record a standby reply on the sender. Returns the
// sender's index on success, -1 if the index is invalid.
int ApplyStandbyReply(int sender_idx, const WalSndReplyMessage& reply);

// GetWalSndStats — aggregate stats across all senders (for tests / metrics).
struct WalSndStats {
    int active_senders = 0;
    int streaming_senders = 0;
    transaction::XLogRecPtr max_sent_lsn = 0;
    transaction::XLogRecPtr max_flush_lsn = 0;
    transaction::XLogRecPtr max_apply_lsn = 0;
};

WalSndStats GetWalSndStats();

}  // namespace pgcpp::replication

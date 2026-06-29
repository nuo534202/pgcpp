// walsenderfuncs.cpp — helper functions used by walsender.c.
//
// Converted from PostgreSQL 15's src/backend/replication/walsender.c
// (the message-handling subset). Network I/O is stubbed; each call
// records the LSN it "would have sent" on the targeted WalSnd and
// returns a small struct describing what was sent.
#include "replication/walsenderfuncs.hpp"

#include "common/error/elog.hpp"
#include "transaction/xlog.hpp"

namespace pgcpp::replication {

using pgcpp::error::LogLevel;

WalSndMessageResult WalSndWriteData(int sender_idx, transaction::XLogRecPtr start_lsn,
                                    transaction::XLogRecPtr end_lsn, bool reply_requested) {
    WalSndMessageResult r;
    r.sender_idx = sender_idx;
    r.start_lsn = start_lsn;
    r.end_lsn = end_lsn;
    r.reply_requested = reply_requested;

    WalSnd* s = WalSndGetByIndex(sender_idx);
    if (s == nullptr) {
        ereport(LogLevel::kError, "WalSndWriteData: invalid sender index");
        return r;
    }
    if (end_lsn < start_lsn) {
        ereport(LogLevel::kError, "WalSndWriteData: end_lsn < start_lsn");
        return r;
    }
    r.bytes_sent = end_lsn - start_lsn;
    if (end_lsn > s->sent_ptr) {
        s->sent_ptr = end_lsn;
    }
    if (reply_requested) {
        s->need_to_flush = true;
    }
    return r;
}

WalSndMessageResult WalSndKeepalive(int sender_idx, bool reply_requested) {
    WalSndMessageResult r;
    r.sender_idx = sender_idx;
    r.reply_requested = reply_requested;

    WalSnd* s = WalSndGetByIndex(sender_idx);
    if (s == nullptr) {
        ereport(LogLevel::kError, "WalSndKeepalive: invalid sender index");
        return r;
    }
    // A keepalive pushes the current insert LSN onto sent_ptr so the
    // standby stays informed even when no real data was sent.
    transaction::XLogRecPtr cur = transaction::GetXLogInsertRecPtr();
    r.start_lsn = s->sent_ptr;
    r.end_lsn = cur;
    if (cur > s->sent_ptr) {
        s->sent_ptr = cur;
    }
    s->need_to_flush = reply_requested;
    return r;
}

int ApplyStandbyReply(int sender_idx, const WalSndReplyMessage& reply) {
    WalSnd* s = WalSndGetByIndex(sender_idx);
    if (s == nullptr) {
        return -1;
    }
    if (reply.write_lsn > s->write_ptr)
        s->write_ptr = reply.write_lsn;
    if (reply.flush_lsn > s->flush_ptr)
        s->flush_ptr = reply.flush_lsn;
    if (reply.apply_lsn > s->apply_ptr)
        s->apply_ptr = reply.apply_lsn;
    if (reply.reply_requested) {
        s->need_to_flush = true;
    } else {
        s->need_to_flush = false;
    }
    return sender_idx;
}

WalSndStats GetWalSndStats() {
    WalSndStats stats;
    WalSndCtlData* ctl = GetWalSndCtl();
    if (ctl == nullptr) {
        return stats;
    }
    stats.active_senders = static_cast<int>(ctl->walsenders.size());
    for (const auto& s : ctl->walsenders) {
        if (s.state == WalSndState::kStreaming) {
            ++stats.streaming_senders;
        }
        if (s.sent_ptr > stats.max_sent_lsn)
            stats.max_sent_lsn = s.sent_ptr;
        if (s.flush_ptr > stats.max_flush_lsn)
            stats.max_flush_lsn = s.flush_ptr;
        if (s.apply_ptr > stats.max_apply_lsn)
            stats.max_apply_lsn = s.apply_ptr;
    }
    return stats;
}

}  // namespace pgcpp::replication

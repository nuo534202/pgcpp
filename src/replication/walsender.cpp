// walsender.cpp — Walsender (streaming-replication server side).
//
// Converted from PostgreSQL 15's src/backend/replication/walsender.c.
//
// PG keeps walsender state in shared memory (WalSndCtl) so that any
// backend can inspect it (e.g. for sync-rep waits). MyToyDB is
// single-process, so we hold a file-static WalSndCtlData singleton.
// Network I/O is stubbed: the LSN accessors and state machine are the
// meaningful surface area.
#include "pgcpp/replication/walsender.hpp"

#include <algorithm>

#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/transaction/xlog.hpp"

namespace mytoydb::replication {

using mytoydb::error::LogLevel;

namespace {

WalSndCtlData& Ctl() {
    static WalSndCtlData c;
    return c;
}

}  // namespace

void WalSndInit() {
    Ctl().walsenders.clear();
    Ctl().lsn_target = 0;
}

void WalSndReset() {
    WalSndInit();
}

WalSndState WalSndGetState(int idx) {
    if (idx < 0 || idx >= static_cast<int>(Ctl().walsenders.size())) {
        return WalSndState::kStopping;
    }
    return Ctl().walsenders[static_cast<std::size_t>(idx)].state;
}

bool WalSndSetState(int idx, WalSndState new_state) {
    if (idx < 0 || idx >= static_cast<int>(Ctl().walsenders.size())) {
        return false;
    }
    Ctl().walsenders[static_cast<std::size_t>(idx)].state = new_state;
    return true;
}

int WalSndAlloc(int32_t pid, std::string application_name) {
    if (static_cast<int>(Ctl().walsenders.size()) >= kMaxWalSenders) {
        ereport(LogLevel::kError, "cannot allocate walsender: max_wal_senders reached");
        return -1;
    }
    WalSnd s;
    s.pid = pid;
    s.application_name = std::move(application_name);
    s.state = WalSndState::kStartup;
    Ctl().walsenders.push_back(std::move(s));
    return static_cast<int>(Ctl().walsenders.size()) - 1;
}

int WalSndGetByPid(int32_t pid) {
    for (std::size_t i = 0; i < Ctl().walsenders.size(); ++i) {
        if (Ctl().walsenders[i].pid == pid) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

WalSnd* WalSndGetByIndex(int idx) {
    if (idx < 0 || idx >= static_cast<int>(Ctl().walsenders.size())) {
        return nullptr;
    }
    return &Ctl().walsenders[static_cast<std::size_t>(idx)];
}

int WalSndCount() {
    return static_cast<int>(Ctl().walsenders.size());
}

void WalSndWakeup() {
    for (auto& s : Ctl().walsenders) {
        s.need_to_flush = true;
    }
}

void WalSndWakeupWaitingForWal(transaction::XLogRecPtr /*lsn*/) {
    // No threads/latches to wake in the stub; just bump the target so a
    // polling caller observes a change in Ctl().lsn_target if needed.
    Ctl().lsn_target = transaction::GetXLogInsertRecPtr();
}

bool WalSndWaitForWal(transaction::XLogRecPtr lsn, int wait_mode, int /*max_iterations*/) {
    // Stubbed wait: PG blocks on a latch and is woken by WalSndWakeup*;
    // MyToyDB is synchronous, so we just verify the LSN is reachable.
    transaction::XLogRecPtr current = transaction::GetXLogInsertRecPtr();
    if (current >= lsn) {
        return true;
    }
    if (wait_mode >= 0) {
        WalSnd* s = WalSndGetByIndex(wait_mode);
        if (s != nullptr && s->flush_ptr >= lsn) {
            return true;
        }
    }
    return false;
}

void WalSndUpdateLsn(int idx, WalSndLsnKind kind, transaction::XLogRecPtr lsn) {
    WalSnd* s = WalSndGetByIndex(idx);
    if (s == nullptr) {
        return;
    }
    switch (kind) {
        case WalSndLsnKind::kSent:
            s->sent_ptr = lsn;
            break;
        case WalSndLsnKind::kWrite:
            s->write_ptr = lsn;
            break;
        case WalSndLsnKind::kFlush:
            s->flush_ptr = lsn;
            break;
        case WalSndLsnKind::kApply:
            s->apply_ptr = lsn;
            break;
    }
}

bool WalSndRemove(int idx) {
    if (idx < 0 || idx >= static_cast<int>(Ctl().walsenders.size())) {
        return false;
    }
    Ctl().walsenders.erase(Ctl().walsenders.begin() + idx);
    return true;
}

WalSndCtlData* GetWalSndCtl() {
    return &Ctl();
}

}  // namespace mytoydb::replication

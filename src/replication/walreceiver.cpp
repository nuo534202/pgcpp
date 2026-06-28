// walreceiver.cpp — Walreceiver (streaming-replication client side).
//
// Converted from PostgreSQL 15's src/backend/replication/walreceiver.c.
// Network code is stubbed: WalRcvStart records the requested conninfo /
// slotname / startpoint and transitions the state to kStreaming.
#include "pgcpp/replication/walreceiver.hpp"

#include "pgcpp/common/error/elog.hpp"

namespace pgcpp::replication {

using pgcpp::error::LogLevel;

namespace {

WalRcvData& Rcv() {
    static WalRcvData r;
    return r;
}

}  // namespace

void WalRcvInit() {
    Rcv() = WalRcvData{};
}

void WalRcvReset() {
    WalRcvInit();
}

bool WalRcvStart(std::string conninfo, std::string slotname, transaction::XLogRecPtr startpoint) {
    if (conninfo.empty()) {
        ereport(LogLevel::kError, "walreceiver start: conninfo is empty");
        return false;
    }
    if (Rcv().state != WalRcvState::kStopped) {
        ereport(LogLevel::kError, "walreceiver start: already running");
        return false;
    }
    Rcv().conninfo = std::move(conninfo);
    Rcv().slotname = std::move(slotname);
    Rcv().startpoint = startpoint;
    Rcv().state = WalRcvState::kStart;
    Rcv().stream_state = WalRcvStreamState::kStreamStart;
    // No fork in pgcpp: skip directly to "streaming".
    Rcv().state = WalRcvState::kStreaming;
    Rcv().receive_ptr = startpoint;
    Rcv().write_ptr = startpoint;
    Rcv().flush_ptr = startpoint;
    Rcv().apply_ptr = startpoint;
    return true;
}

int32_t WalRcvStop() {
    if (Rcv().state == WalRcvState::kStopped) {
        return 0;
    }
    int32_t pid = Rcv().pid;
    Rcv().state = WalRcvState::kStopped;
    Rcv().stream_state = WalRcvStreamState::kNone;
    Rcv().pid = 0;
    return pid;
}

WalRcvState WalRcvGetState() {
    return Rcv().state;
}

WalRcvStreamState WalRcvGetStreamState() {
    return Rcv().stream_state;
}

const char* WalRcvStateName(WalRcvState s) {
    switch (s) {
        case WalRcvState::kStopped:
            return "stopped";
        case WalRcvState::kStart:
            return "starting";
        case WalRcvState::kStreaming:
            return "streaming";
    }
    return "unknown";
}

void WalRcvSetPid(int32_t pid) {
    Rcv().pid = pid;
}

void WalRcvReportLsn(WalRcvLsnKind kind, transaction::XLogRecPtr lsn) {
    switch (kind) {
        case WalRcvLsnKind::kReceive:
            Rcv().receive_ptr = lsn;
            break;
        case WalRcvLsnKind::kWrite:
            Rcv().write_ptr = lsn;
            break;
        case WalRcvLsnKind::kFlush:
            Rcv().flush_ptr = lsn;
            break;
        case WalRcvLsnKind::kApply:
            Rcv().apply_ptr = lsn;
            break;
    }
}

WalRcvData* GetWalRcvData() {
    return &Rcv();
}

}  // namespace pgcpp::replication

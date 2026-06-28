// replutil.cpp — Shared utility types for the replication subsystem.
//
// Converted from PostgreSQL 15's src/backend/replication/replutil.c (and
// the message-type enum in libpq/pqformat.h). Network/IPC parts are
// stubbed; only the type table and the wal_level accessor live here.
#include "mytoydb/replication/replutil.hpp"

namespace mytoydb::replication {

namespace {

WalLevel& GlobalWalLevel() {
    static WalLevel level = WalLevel::kReplica;
    return level;
}

}  // namespace

const char* LogicalRepMsgTypeName(LogicalRepMsgType t) {
    switch (t) {
        case LogicalRepMsgType::kBegin:
            return "BEGIN";
        case LogicalRepMsgType::kMessage:
            return "MESSAGE";
        case LogicalRepMsgType::kCommit:
            return "COMMIT";
        case LogicalRepMsgType::kOrigin:
            return "ORIGIN";
        case LogicalRepMsgType::kRelation:
            return "RELATION";
        case LogicalRepMsgType::kType:
            return "TYPE";
        case LogicalRepMsgType::kInsert:
            return "INSERT";
        case LogicalRepMsgType::kUpdate:
            return "UPDATE";
        case LogicalRepMsgType::kDelete:
            return "DELETE";
        case LogicalRepMsgType::kTruncate:
            return "TRUNCATE";
        case LogicalRepMsgType::kStreamStart:
            return "STREAM START";
        case LogicalRepMsgType::kStreamStop:
            return "STREAM STOP";
        case LogicalRepMsgType::kStreamCommit:
            return "STREAM COMMIT";
        case LogicalRepMsgType::kStreamAbort:
            return "STREAM ABORT";
    }
    return "UNKNOWN";
}

WalLevel GetWalLevel() {
    return GlobalWalLevel();
}

void SetWalLevel(WalLevel level) {
    GlobalWalLevel() = level;
}

const char* DefaultOutputPluginName() {
    return "pgoutput";
}

}  // namespace mytoydb::replication

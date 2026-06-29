// origin.cpp — Replication origin tracking.
//
// Converted from PostgreSQL 15's src/backend/replication/logical/origin.c.
// PG stores origins in pg_replication_origin catalog; pgcpp keeps an
// in-process std::map<uint16, CommitOrigin> and tracks the current
// session's origin id and LSN.
#include "replication/origin.hpp"

#include <map>
#include <string>

#include "common/error/elog.hpp"
#include "transaction/xlog.hpp"

namespace pgcpp::replication {

using pgcpp::error::LogLevel;

namespace {

struct OriginStore {
    std::map<RepOriginId, CommitOrigin> by_id;
    std::map<std::string, RepOriginId> by_name;
    RepOriginId next_id = kFirstUserRepOriginId;
    RepOriginId session_id = kInvalidRepOriginId;
    transaction::XLogRecPtr session_local_lsn = 0;
};

OriginStore& Store() {
    static OriginStore s;
    return s;
}

}  // namespace

void ReplOriginInit() {
    Store() = OriginStore{};
}

void ReplOriginReset() {
    ReplOriginInit();
}

RepOriginId ReploriginCreate(const std::string& name) {
    if (name.empty()) {
        ereport(LogLevel::kError, "replorigin_create: name is empty");
        return kInvalidRepOriginId;
    }
    auto it = Store().by_name.find(name);
    if (it != Store().by_name.end()) {
        return it->second;
    }
    RepOriginId id = Store().next_id++;
    CommitOrigin o;
    o.id = id;
    o.name = name;
    Store().by_id[id] = o;
    Store().by_name[name] = id;
    return id;
}

bool ReploriginDrop(RepOriginId id) {
    auto it = Store().by_id.find(id);
    if (it == Store().by_id.end()) {
        return false;
    }
    std::string name = it->second.name;
    Store().by_name.erase(name);
    Store().by_id.erase(it);
    if (Store().session_id == id) {
        Store().session_id = kInvalidRepOriginId;
    }
    return true;
}

bool ReploriginDropByName(const std::string& name) {
    auto it = Store().by_name.find(name);
    if (it == Store().by_name.end()) {
        return false;
    }
    return ReploriginDrop(it->second);
}

bool ReploriginAdvance(RepOriginId id, transaction::XLogRecPtr remote_lsn,
                       transaction::XLogRecPtr local_lsn) {
    auto it = Store().by_id.find(id);
    if (it == Store().by_id.end()) {
        return false;
    }
    if (remote_lsn > it->second.remote_lsn) {
        it->second.remote_lsn = remote_lsn;
    }
    if (local_lsn > it->second.local_lsn) {
        it->second.local_lsn = local_lsn;
    }
    return true;
}

bool ReploriginSessionSet(RepOriginId id) {
    auto it = Store().by_id.find(id);
    if (it == Store().by_id.end()) {
        ereport(LogLevel::kError, "replorigin_session_set: origin does not exist");
        return false;
    }
    Store().session_id = id;
    Store().session_local_lsn = 0;
    return true;
}

RepOriginId ReploriginSessionGet() {
    return Store().session_id;
}

void ReploriginSessionReset() {
    Store().session_id = kInvalidRepOriginId;
    Store().session_local_lsn = 0;
}

const CommitOrigin* ReploriginGet(RepOriginId id) {
    auto it = Store().by_id.find(id);
    if (it == Store().by_id.end()) {
        return nullptr;
    }
    return &it->second;
}

RepOriginId ReploriginGetByName(const std::string& name) {
    auto it = Store().by_name.find(name);
    if (it == Store().by_name.end()) {
        return kInvalidRepOriginId;
    }
    return it->second;
}

int ReploriginCount() {
    return static_cast<int>(Store().by_id.size());
}

transaction::XLogRecPtr ReploriginSessionLsn() {
    return Store().session_local_lsn;
}

}  // namespace pgcpp::replication

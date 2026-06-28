// backup.cpp — Base backup API.
//
// Converted from PostgreSQL 15's src/backend/replication/backup.c.
// File-copy work is stubbed: DoBackup just records entries on the
// BackupHandle, no actual I/O happens.
#include "pgcpp/replication/backup.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/transaction/xlog.hpp"

namespace mytoydb::replication {

using mytoydb::error::LogLevel;

namespace {

BackupHandle& Active() {
    static BackupHandle h;
    return h;
}

bool& HasActive() {
    static bool b = false;
    return b;
}

}  // namespace

void InitializeBackup() {
    Active() = BackupHandle{};
    HasActive() = false;
}

void ResetBackup() {
    InitializeBackup();
}

BackupHandle StartBackup(const std::string& label) {
    if (HasActive()) {
        ereport(LogLevel::kError, "StartBackup: a backup is already running");
        return BackupHandle{};
    }
    BackupHandle h;
    h.label = label;
    h.start_lsn = transaction::GetXLogInsertRecPtr();
    h.state = BackupState::kRunning;
    Active() = h;
    HasActive() = true;
    return h;
}

int DoBackup(BackupHandle& handle, const std::vector<std::pair<std::string, std::size_t>>& files) {
    if (handle.state != BackupState::kRunning) {
        ereport(LogLevel::kError, "DoBackup: backup is not running");
        return 0;
    }
    int written = 0;
    for (const auto& f : files) {
        handle.files.push_back(f);
        handle.total_bytes += f.second;
        ++written;
    }
    return written;
}

transaction::XLogRecPtr StopBackup(BackupHandle& handle, bool /*exclusive*/) {
    if (handle.state != BackupState::kRunning) {
        ereport(LogLevel::kError, "StopBackup: backup is not running");
        return 0;
    }
    transaction::XLogRecPtr end_lsn = transaction::GetXLogInsertRecPtr();
    handle.state = BackupState::kDone;
    // Only one backup can be active at a time (enforced by StartBackup), so
    // stopping any running backup clears the active flag. The handle passed
    // in may be a copy of the active singleton (StartBackup returns by
    // value), so a pointer comparison would miss it.
    if (HasActive()) {
        HasActive() = false;
    }
    return end_lsn;
}

BackupHandle* GetCurrentBackup() {
    if (!HasActive()) {
        return nullptr;
    }
    return &Active();
}

const char* BackupStateName(BackupState s) {
    switch (s) {
        case BackupState::kNotStarted:
            return "not_started";
        case BackupState::kRunning:
            return "running";
        case BackupState::kDone:
            return "done";
        case BackupState::kCancelled:
            return "cancelled";
    }
    return "unknown";
}

}  // namespace mytoydb::replication

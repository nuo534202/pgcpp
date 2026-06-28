// backup.h — Base backup API.
//
// Converted from PostgreSQL 15's src/backend/replication/backup.c
// (the `pg_basebackup`-style server-side helpers: do_pg_backup_start,
// do_pg_backup_stop, perform_base_backup). MyToyDB stubs the actual
// file-copy work and exposes StartBackup / StopBackup / DoBackup so
// other modules can drive a "consistent snapshot" lifecycle.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "mytoydb/transaction/xlog.hpp"

namespace mytoydb::replication {

// BackupState — lifecycle state of one base backup.
enum class BackupState : uint8_t {
    kNotStarted = 0,
    kRunning = 1,
    kDone = 2,
    kCancelled = 3,
};

// BackupHandle — opaque handle returned by StartBackup.
struct BackupHandle {
    std::string label;                      // user-supplied label (e.g. "my backup")
    transaction::XLogRecPtr start_lsn = 0;  // LSN at which backup began
    BackupState state = BackupState::kNotStarted;
    // Files "touched" by the stub backup (just names + sizes; no I/O).
    std::vector<std::pair<std::string, std::size_t>> files;
    // Total bytes the stub "would have" written.
    std::size_t total_bytes = 0;
};

// InitializeBackup — clear the backup subsystem.
void InitializeBackup();

// ResetBackup — alias for tests.
void ResetBackup();

// StartBackup — begin a base backup with the given label. Returns a
// handle that the caller drives through DoBackup / StopBackup.
// On error (already running), calls ereport(ERROR).
BackupHandle StartBackup(const std::string& label);

// DoBackup — perform the (stubbed) file-copy work. Returns the number of
// files "written". Just records entries on the handle.
int DoBackup(BackupHandle& handle, const std::vector<std::pair<std::string, std::size_t>>& files);

// StopBackup — finalize the backup and return its end LSN. The handle's
// state transitions to kDone (or kCancelled on error).
transaction::XLogRecPtr StopBackup(BackupHandle& handle, bool exclusive = true);

// GetCurrentBackup — return a pointer to the currently-running backup
// handle, or nullptr if no backup is active.
BackupHandle* GetCurrentBackup();

// BackupStateName — string form of a BackupState.
const char* BackupStateName(BackupState s);

}  // namespace mytoydb::replication

// pgarch.h — WAL archiver: archives completed WAL segment files.
//
// Converted from PostgreSQL 15's src/backend/postmaster/pgarch.c.
//
// PostgreSQL archives each completed WAL segment file (16 MB) by invoking
// an external archive_command (configured in postgresql.conf) with the
// file path. The archiver runs in a loop, polling for new segment files
// in pg_wal/ and archiving them one at a time. This enables point-in-time
// recovery and replication.
//
// In pgcpp (single-process), the archiver is a stateful API that maintains
// a queue of pending archive requests. PgArchiverMain processes the queue
// by calling ShellArchive for each file.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pgcpp::server {

// PgArchState — state of the WAL archiver.
enum class PgArchState {
    // kStopped — archiver not started.
    kStopped,
    // kRunning — archiver in main loop, waiting for files.
    kRunning,
    // kArchiving — archiver currently archiving a file.
    kArchiving,
};

// PgArchStats — statistics tracked by the WAL archiver.
struct PgArchStats {
    // Number of files successfully archived.
    uint64_t files_archived = 0;
    // Number of archive failures.
    uint64_t archive_failures = 0;
    // Timestamp (ms since epoch) of the last successful archive.
    int64_t last_archive_time_ms = 0;
    // Name of the last file archived.
    std::string last_archived_file;
    // Whether the archiver is currently running its main loop.
    bool running = false;
};

// InitializePgArch — set up archiver state (clear queue and stats).
void InitializePgArch();

// ResetPgArch — clear archiver state, queue, and statistics (for testing).
void ResetPgArch();

// PgArchStart — start the WAL archiver.
void PgArchStart();

// PgArchStop — stop the WAL archiver.
void PgArchStop();

// PgArchiverMain — main loop of the archiver (simplified: processes
// pending archive requests up to `max_iterations` of them). Returns the
// number of files archived.
int PgArchiverMain(int max_iterations);

// PgArchiveWALFile — archive a single WAL segment file.
// Calls the registered archive command (see shell_archive.h). Returns true
// on success, false on failure.
bool PgArchiveWALFile(const std::string& file_path);

// QueueArchiveRequest — queue a WAL segment for archiving.
// Returns true if the file was added, false if already queued.
bool QueueArchiveRequest(const std::string& file_path);

// GetPendingArchiveRequests — return a copy of the pending archive queue.
std::vector<std::string> GetPendingArchiveRequests();

// GetPgArchState — return the current archiver state.
PgArchState GetPgArchState();

// GetPgArchStats — return the current archiver statistics.
PgArchStats GetPgArchStats();

}  // namespace pgcpp::server

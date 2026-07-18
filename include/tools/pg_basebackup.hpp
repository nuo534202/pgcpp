// pg_basebackup.h — Base backup utility (pg_basebackup).
//
// Converted from PostgreSQL 15's src/bin/pg_basebackup/.
//
// PG's pg_basebackup connects to a server via the replication protocol,
// issues BASE_BACKUP, and receives a tar stream. pgcpp's storage model
// is a local data directory, so the simplified port instead takes a
// coordinated filesystem-level snapshot of a stopped-or-checkpointed data
// directory, copying it (file-by-file) to a destination directory.
//
// The resulting backup is a logical equivalent of a base backup: it can
// be used as the starting point for a new server or for PITR. The copy
// excludes postmaster.pid, postmaster.opts, and log files (which are
// runtime artifacts). pg_wal/ is copied only when --wal-method=fetch,
// matching PG's behavior.
#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace pgcpp::tools {

// WalMethod — how to capture WAL generated during the backup.
enum class WalMethod {
    kNone,    // don't capture WAL
    kFetch,   // copy pg_wal/ into the backup at the end (default)
    kStream,  // open a separate walreceiver (not supported by pgcpp)
};

// BasebackupOptions — inputs to the backup.
struct BasebackupOptions {
    // Source data directory (-D source or -D path on the server side).
    // In pgcpp we don't speak the replication protocol, so we copy
    // directly from this local path.
    std::string source_dir;
    // Destination directory for the backup.
    std::string target_dir;
    // Connection info (unused for local file copy, kept for parity).
    std::string host = "localhost";
    int port = 5432;
    std::string user;
    std::string dbname;
    // WAL handling.
    WalMethod wal_method = WalMethod::kFetch;
    // Force a checkpoint before copying (in pgcpp, this is a no-op since
    // we don't speak the protocol; kept for parity).
    bool checkpoint = false;
    // Print progress (file count, bytes) at the end.
    bool progress = false;
    // Verbose output.
    bool verbose = false;
    // Compression (gzip). pgcpp compresses individual files when set.
    bool gzip = false;
    int compression_level = 6;
    // Don't actually copy; just compute and report stats.
    bool dry_run = false;
};

// BasebackupResult — outcome of a backup.
enum class BasebackupResult {
    kOk,
    kInvalidSourceDir,  // source missing or not a data directory
    kInvalidTargetDir,  // target dir is invalid or non-empty
    kSourceIsTarget,    // source and target resolve to the same path
    kCopyFailed,        // I/O error during copy
    kNoSpaceLeft,       // disk full
};

// BasebackupStats — counters accumulated during a backup.
struct BasebackupStats {
    int files_copied = 0;
    int files_skipped = 0;
    std::int64_t bytes_copied = 0;
    std::int64_t bytes_skipped = 0;
    int wal_files_copied = 0;
    std::int64_t wal_bytes_copied = 0;
    int errors = 0;
};

// RunBasebackup — perform the backup described by `opts`.
BasebackupResult RunBasebackup(const BasebackupOptions& opts, BasebackupStats& stats,
                               std::ostream* verbose_out = nullptr);

// --- Helpers (exposed for testing) ---

// IsDataDir — true if `path` looks like a pgcpp data directory. We test
// for the presence of "PG_VERSION" or "postgresql.conf" (case-sensitive).
bool IsDataDir(const std::string& path);

// ShouldSkipFile — true for files that must not be copied into the backup
// (postmaster.pid, postmaster.opts, log files, socket files, etc.).
bool ShouldSkipFile(const std::string& basename);

// IsWalFile — true for files under pg_wal/ (16MB segment files plus
// archive_status/ subdirectory).
bool IsWalFile(const std::string& relpath);

// FormatBytes — human-readable byte count (KiB/MiB/GiB).
std::string FormatBytes(std::int64_t bytes);

// EnsureTargetDir — create the target directory if it doesn't exist.
// Returns false if it already exists and is non-empty.
bool EnsureTargetDir(const std::string& path);

}  // namespace pgcpp::tools

// pg_rewind.h — Data directory synchronizer (pg_rewind).
//
// Converted from PostgreSQL 15's src/bin/pg_rewind/.
//
// PG's pg_rewind synchronizes a stale PostgreSQL data directory (target)
// with a newer copy (source) by:
//   1. Reading the source's control file to find the divergence point.
//   2. Scanning WAL to find every block modified since divergence.
//   3. Copying modified blocks and new files from source to target.
//   4. Removing files no longer present in the source.
//
// pgcpp's storage model is a local data directory; the simplified port
// does a content-based file-level sync instead of WAL replay:
//   - Walk both trees and produce a relative-path list.
//   - For each path that exists in both, copy source→target if their
//     SHA-1 (or size, if --quick) differs.
//   - For each path that exists only in source, copy it.
//   - For each path that exists only in target, remove it.
//
// Runtime artifacts (postmaster.pid, log files, sockets) are skipped on
// both sides (matching pg_basebackup's skip rules).
#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace pgcpp::tools {

// RewindOptions — inputs to a rewind.
struct RewindOptions {
    std::string source_dir;  // the newer data directory
    std::string target_dir;  // the stale data directory to be synced
    bool dry_run = false;    // don't actually copy/remove, just report
    bool verbose = false;
    bool quick = false;    // compare sizes only (skip content hash)
    bool no_sync = false;  // don't fsync the target files (parity flag)
};

// RewindResult — outcome of a rewind.
enum class RewindResult {
    kOk,
    kInvalidSourceDir,
    kInvalidTargetDir,
    kSourceIsTarget,
    kSourceNotNewer,  // source's control file is older than target's
    kCopyFailed,
    kRemoveFailed,
};

// RewindStats — counters accumulated during a rewind.
struct RewindStats {
    int files_copied = 0;
    int files_removed = 0;
    int files_unchanged = 0;
    std::int64_t bytes_copied = 0;
    int errors = 0;
};

// RunRewind — synchronize target_dir to match source_dir.
RewindResult RunRewind(const RewindOptions& opts, RewindStats& stats,
                       std::ostream* verbose_out = nullptr);

// --- Helpers (exposed for testing) ---

// IsDataDir — true if `path` looks like a pgcpp data directory.
// (Delegates to pg_basebackup's IsDataDir to avoid duplication.)
bool IsDataDir(const std::string& path);

// ShouldSkipFile — true for files that must not be synced (postmaster.pid,
// postmaster.opts, log files, socket files). Same rules as pg_basebackup.
bool ShouldSkipFile(const std::string& basename);

// ComputeFileHash — 64-bit FNV-1a hash of file contents (0 if file missing).
std::uint64_t ComputeFileHash(const std::string& path);

// FilesDiffer — true if the two files differ in size or content.
// When `quick` is set, only the size is compared.
bool FilesDiffer(const std::string& path1, const std::string& path2, bool quick);

// EnumerateDataDirFiles — walk `path` recursively and return a sorted list
// of relative paths (relative to `path`). Skips runtime artifacts.
std::vector<std::string> EnumerateDataDirFiles(const std::string& path);

// SyncFile — copy `src` to `dst`, creating parent directories as needed.
bool SyncFile(const std::string& src, const std::string& dst, std::int64_t* bytes_out);

// RemoveFile — remove a file (or empty directory) at `path`.
bool RemoveFile(const std::string& path);

// EnsureParentDir — create the parent directory of `path` if missing.
bool EnsureParentDir(const std::string& path);

}  // namespace pgcpp::tools

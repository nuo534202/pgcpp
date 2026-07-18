// pg_upgrade.h — Cluster upgrade utility (pg_upgrade).
//
// Converted from PostgreSQL 15's src/bin/pg_upgrade/.
//
// PG's pg_upgrade upgrades a PostgreSQL cluster from one major version to
// another by:
//   1. Stopping both old and new clusters.
//   2. Verifying compatibility (pg_control, system catalogs, etc.).
//   3. Copying (or linking, or cloning) the user data files from the
//      old data directory into the new one (skipping system catalogs).
//   4. Updating the new cluster's system catalogs to point at the new
//      file layout.
//
// pgcpp doesn't have multi-version cluster management, so this port is
// a compatibility check + relation-file migration between two local data
// directories. The PG_VERSION file is the source of truth for "what
// version is this cluster".
#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace pgcpp::tools {

// UpgradeMode — how to copy relation files from old → new.
enum class UpgradeMode {
    kCopy,   // full file copy (default)
    kLink,   // hard link (fastest, but old & new must be on same FS)
    kClone,  // reflink / file_clone (Linux: FICLONE)
};

// UpgradeOptions — inputs to an upgrade.
struct UpgradeOptions {
    std::string old_dir;  // old (source) data directory
    std::string new_dir;  // new (target) data directory
    UpgradeMode mode = UpgradeMode::kCopy;
    bool check_only = false;  // don't actually migrate, just verify
    bool verbose = false;
    int jobs = 1;  // ignored by pgcpp (single-threaded)
};

// UpgradeResult — outcome of an upgrade.
enum class UpgradeResult {
    kOk,
    kInvalidOldDir,
    kInvalidNewDir,
    kSameDirectory,
    kVersionMismatch,     // old/new major versions are not upgradeable
    kNewClusterNotEmpty,  // new_dir contains user data
    kOldClusterRunning,   // old_dir has postmaster.pid (server still up)
    kCopyFailed,
    kCloneUnsupported,  // reflink not available on this filesystem
};

// UpgradeStats — counters accumulated during an upgrade.
struct UpgradeStats {
    int files_copied = 0;
    int files_linked = 0;
    int files_cloned = 0;
    int files_skipped = 0;
    std::int64_t bytes_migrated = 0;
    int errors = 0;
};

// RunUpgrade — verify and (unless --check) migrate the cluster.
UpgradeResult RunUpgrade(const UpgradeOptions& opts, UpgradeStats& stats,
                         std::ostream* verbose_out = nullptr);

// --- Helpers (exposed for testing) ---

// ReadVersionFile — read the major version number from <dir>/PG_VERSION.
// Returns 0 on error or if the file is missing/malformed.
int ReadVersionFile(const std::string& dir);

// CheckCompatibility — true if upgrading from `old_version` to
// `new_version` is supported by pgcpp. pgcpp only allows same-major-version
// "upgrades" (a no-op) since we don't have a real catalog migration
// pipeline.
bool CheckCompatibility(int old_version, int new_version);

// IsClusterRunning — true if <dir>/postmaster.pid exists.
bool IsClusterRunning(const std::string& dir);

// NewClusterHasUserData — true if <dir>/base/<db_oid>/ has any non-empty
// relation files (i.e. the user has already loaded data into the new
// cluster, which would be clobbered by an upgrade).
bool NewClusterHasUserData(const std::string& dir);

// CopyRelationFile — copy/link/clone a single relation file.
// `mode` selects the strategy. Returns true on success. On kClone, may
// return false with errno=EXDEV or ENOTSUP if the filesystem doesn't
// support reflinks (caller should retry with kCopy).
bool CopyRelationFile(const std::string& src, const std::string& dst, UpgradeMode mode,
                      std::int64_t* bytes_out);

// IsRelationFile — true if `basename` looks like a relation file (numeric
// name, optional _fsm/_vm/_init suffix). Matches pg_checksums' heuristic.
bool IsRelationFile(const std::string& basename);

}  // namespace pgcpp::tools

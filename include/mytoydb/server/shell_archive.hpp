// shell_archive.h — Shell-based WAL segment archiving.
//
// Converted from PostgreSQL 15's src/backend/postmaster/shell_archive.c.
//
// PostgreSQL archives each completed WAL segment by invoking an external
// shell command (configured via archive_command in postgresql.conf). The
// command receives two arguments substituted by the server:
//   %p — the absolute path to the WAL segment file to archive
//   %f — the file name (basename) of the WAL segment
// If the command exits with status 0, archiving is considered successful;
// any non-zero exit status causes a retry on the next archiver cycle.
//
// In MyToyDB, ShellArchive is a function that takes a file path and a
// "last" identifier (used by tests to verify substitution) and executes
// the configured archive command via the system shell.
#pragma once

#include <cstdint>
#include <string>

namespace mytoydb::server {

// ShellArchiveStats — statistics tracked by shell archiving.
struct ShellArchiveStats {
    // Number of files successfully archived.
    uint64_t files_archived = 0;
    // Number of archive failures (non-zero exit status).
    uint64_t failures = 0;
    // Timestamp (ms since epoch) of the last successful archive.
    int64_t last_archive_time_ms = 0;
    // Name of the last file archived.
    std::string last_archived_file;
    // Exit code of the last archive command.
    int last_exit_code = 0;
};

// InitializeShellArchive — set up shell archiver state.
void InitializeShellArchive();

// ResetShellArchive — clear shell archiver state and stats (for testing).
void ResetShellArchive();

// SetArchiveCommand — set the shell command template used for archiving.
// The placeholder %p is replaced with the file path, %f with the basename.
// If set to empty, archiving is disabled (ShellArchive returns 0 without
// running anything).
void SetArchiveCommand(const std::string& cmd_template);

// GetArchiveCommand — return the current archive command template.
std::string GetArchiveCommand();

// IsArchiveCommandSet — true if an archive command is configured.
bool IsArchiveCommandSet();

// ShellArchive — archive a WAL segment by executing the archive command.
// Substitutes %p with `file_path` and %f with the basename of `file_path`,
// then invokes the resulting command via the system shell.
//
// Parameters:
//   file_path — absolute path to the WAL segment file to archive.
//   last — the previous archived segment name (for testing/diagnostics;
//          not used in command substitution).
//
// Returns 0 on success (command exited 0), non-zero on failure (command
// failed or no command configured).
int ShellArchive(const std::string& file_path, const std::string& last);

// GetShellArchiveStats — return the current shell archive statistics.
ShellArchiveStats GetShellArchiveStats();

}  // namespace mytoydb::server

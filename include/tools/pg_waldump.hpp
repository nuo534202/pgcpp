// pg_waldump.h — WAL record dumper (pg_waldump).
//
// Converted from PostgreSQL 15's src/bin/pg_waldump/.
//
// pg_waldump reads a WAL stream and prints one line per XLogRecord, showing
// the record's LSN, prev-LSN, xid, resource manager, info byte, and total
// length. It is primarily used to inspect what WAL records a transaction
// produced.
//
// pgcpp's pg_waldump supports two input modes:
//   1. In-memory WAL buffer (default) — reads from GetWalBuffer().
//   2. WAL file path — reads raw bytes from a file on disk.
//
// Usage:
//   pg_waldump [--start <lsn>] [--end <lsn>] [--path <wal_file>]
//              [--rmgr <name>] [--limit <n>] [--stats]
#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

#include "transaction/xlog.hpp"

namespace pgcpp::tools {

// WaldumpOptions — inputs to DumpWal.
struct WaldumpOptions {
    // LSN to start reading from (inclusive). 0 = start of WAL.
    pgcpp::transaction::XLogRecPtr start_lsn = 0;

    // LSN to stop reading at (exclusive). 0 = end of WAL.
    pgcpp::transaction::XLogRecPtr end_lsn = 0;

    // File path to read WAL from. Empty = read from in-memory WAL buffer.
    std::string path;

    // Filter: only print records from this resource manager (by name).
    // Empty = print records from all resource managers.
    std::string rmgr_filter;

    // Maximum number of records to print (0 = no limit).
    std::size_t limit = 0;

    // If true, print a per-resource-manager statistics summary instead of
    // per-record lines.
    bool stats = false;
};

// WaldumpResult — outcome of a dump run.
enum class WaldumpResult {
    kOk,               // normal completion
    kOpenFailed,       // could not open the WAL file
    kReadFailed,       // read error (short record / truncated)
    kInvalidArgument,  // bad LSN or unknown rmgr filter
};

// WaldumpStats — per-resource-manager statistics accumulated by DumpWal.
struct WaldumpStats {
    // Resource manager name (e.g. "XLOG", "Transaction").
    std::string rmgr_name;

    // Number of records seen for this rmgr.
    std::size_t count = 0;

    // Total bytes of record data (excluding the 24-byte header) for this rmgr.
    std::uint64_t total_len = 0;
};

// RmgrName — return the human-readable name of resource manager `rmid`.
// Returns "Unknown" for unrecognized IDs.
const char* RmgrName(pgcpp::transaction::RmgrId rmid);

// RmgrIdFromName — look up a resource manager ID by name (case-insensitive).
// Returns true and sets `out` on success, false if the name is unknown.
bool RmgrIdFromName(const std::string& name, pgcpp::transaction::RmgrId& out);

// FormatLsn — format an LSN as "X/Y" (PostgreSQL's standard hex notation).
std::string FormatLsn(pgcpp::transaction::XLogRecPtr lsn);

// ParseLsn — parse an "X/Y" or hexadecimal LSN string. Returns true on success.
bool ParseLsn(const std::string& s, pgcpp::transaction::XLogRecPtr& out);

// DumpWal — read WAL records and print a one-line summary per record to `out`.
// Honors the options (start/end LSN, rmgr filter, limit). Returns the outcome
// and (when opts.stats is true) accumulates per-rmgr statistics in `stats`.
WaldumpResult DumpWal(const WaldumpOptions& opts, std::ostream& out,
                      std::vector<WaldumpStats>* stats = nullptr);

}  // namespace pgcpp::tools

// pg_restore.h — Database restore utility (pg_restore).
//
// Converted from PostgreSQL 15's src/bin/pg_restore/.
//
// pg_restore reads a dump file (produced by pg_dump) and replays it against
// a running server. PG's pg_restore supports the custom archive format with
// parallel restore, selective object restore, etc.
//
// pgcpp's pg_restore is a simplified version that:
//   - Reads a plain SQL dump file (the only format pg_dump produces).
//   - Splits it into statements (splitting on ';' at end-of-line, respecting
//     $$ ... $$ dollar-quoting and COPY ... FROM stdin blocks).
//   - Executes each statement via PsqlClient::ExecuteQuery.
//   - Reports any errors but continues (matching PG's default behaviour).
#pragma once

#include <istream>
#include <string>
#include <vector>

#include "tools/psql_client.hpp"

namespace pgcpp::tools {

// RestoreOptions — controls how pg_restore replays the dump.
struct RestoreOptions {
    // Database to restore into (also the connection DB).
    std::string database;
    // Stop on first error (default false).
    bool exit_on_error = false;
    // Only emit DDL (skip COPY/INSERT data).
    bool schema_only = false;
    // Skip DDL (only replay COPY/INSERT data).
    bool data_only = false;
};

// RestoreResult — outcome of a restore.
enum class RestoreResult {
    kOk,
    kConnectFailed,
    kReadFailed,
    kStatementFailed,  // one or more statements failed
};

// RestoreDump — read SQL statements from `in` and execute them via `client`.
RestoreResult RestoreDump(PsqlClient& client, std::istream& in, const RestoreOptions& opts);

// RestoreDumpFromFile — convenience wrapper that opens `path` and calls
// RestoreDump.
RestoreResult RestoreDumpFromFile(PsqlClient& client, const std::string& path,
                                  const RestoreOptions& opts);

// --- Helpers (exposed for testing) ---

// SplitDumpIntoStatements — split a SQL dump into individual statements.
// Handles:
//   - ';' as statement terminator (outside string literals and dollar-quoted
//     blocks).
//   - COPY ... FROM stdin;\n<rows>\n\\. as a single statement (including the
//     data rows and the terminator line).
// Returns the list of statements (each is the full text, including the
// trailing ';' for non-COPY statements).
std::vector<std::string> SplitDumpIntoStatements(const std::string& dump);

// IsCopyStatement — true if `stmt` starts with COPY (case-insensitive) and
// ends with "FROM stdin;".
bool IsCopyStatement(const std::string& stmt);

// IsDataStatement — true if `stmt` is a COPY or INSERT statement (i.e.
// produces data, not DDL).
bool IsDataStatement(const std::string& stmt);

}  // namespace pgcpp::tools

// pg_dump.h — Database dump utility (pg_dump).
//
// Converted from PostgreSQL 15's src/bin/pg_dump/.
//
// pg_dump connects to a running server, queries the system catalog, and
// writes a SQL script that can be replayed by psql (or pg_restore) to
// recreate the schema and/or data.
//
// PG's pg_dump is a 10000-line tool with support for parallel dumps, custom
// archive format, tar format, etc. pgcpp provides a simplified text-format
// dump that:
//   - Queries pg_class for tables/views/sequences.
//   - Emits CREATE TABLE statements with column definitions.
//   - Emits COPY ... FROM stdin blocks for table data (when --data-only is
//     not set and --schema-only is not set).
//   - Emits CREATE INDEX statements.
//
// The output is a plain SQL text file.
#pragma once

#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "tools/psql_client.hpp"

namespace pgcpp::tools {

// DumpFormat — output format (PG supports custom/tar/plain/dir; pgcpp plain only).
enum class DumpFormat {
    kPlain,   // SQL text (the only format pgcpp supports)
    kCustom,  // PG custom archive (not implemented in pgcpp)
};

// DumpOptions — controls what pg_dump emits.
struct DumpOptions {
    // Database to dump (also used as the connection DB).
    std::string database;
    // Only dump tables whose name matches this pattern (empty = all).
    std::string table_pattern;
    // Schema-only (skip data).
    bool schema_only = false;
    // Data-only (skip schema).
    bool data_only = false;
    // Include DROP statements before CREATE.
    bool clean = false;
    // Use INSERT statements instead of COPY for data.
    bool inserts = false;
    // Dump format.
    DumpFormat format = DumpFormat::kPlain;
};

// DumpResult — outcome of a dump.
enum class DumpResult {
    kOk,
    kConnectFailed,
    kCatalogQueryFailed,
    kNoTablesFound,
};

// DumpDatabase — connect to the server and write a SQL dump to `out`.
DumpResult DumpDatabase(const std::string& host, int port, const DumpOptions& opts,
                        std::ostream& out);

// --- Helpers (exposed for testing) ---

// BuildDropTableStatement — emit "DROP TABLE IF EXISTS <name>;".
std::string BuildDropTableStatement(const std::string& table_name);

// BuildCreateTableStatement — emit a CREATE TABLE statement from column
// definitions. `columns` is a vector of (name, type) pairs.
std::string BuildCreateTableStatement(
    const std::string& table_name, const std::vector<std::pair<std::string, std::string>>& columns);

// BuildCopyHeader — emit "COPY <table> (<cols>) FROM stdin;".
std::string BuildCopyHeader(const std::string& table_name,
                            const std::vector<std::string>& column_names);

// BuildInsertStatement — emit "INSERT INTO <table> (<cols>) VALUES (...);".
std::string BuildInsertStatement(const std::string& table_name,
                                 const std::vector<std::string>& column_names,
                                 const std::vector<std::string>& values);

// QuoteIdentifier — double-quote an identifier (doubling embedded quotes).
std::string QuoteIdentifier(const std::string& s);

// QuoteLiteral — single-quote a string literal (doubling embedded quotes).
std::string QuoteLiteral(const std::string& s);

}  // namespace pgcpp::tools

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

// BuildCreateIndexStatement — emit "CREATE [UNIQUE] INDEX <name> ON <table> (<cols>);".
// `unique` controls whether UNIQUE is emitted. `columns` is a list of column
// names that are joined with ", ".
std::string BuildCreateIndexStatement(const std::string& index_name, const std::string& table_name,
                                      const std::vector<std::string>& columns, bool unique);

// BuildDropIndexStatement — emit "DROP INDEX IF EXISTS <name>;".
std::string BuildDropIndexStatement(const std::string& index_name);

// BuildCreateSequenceStatement — emit "CREATE SEQUENCE <name> [AS type]
// [START WITH n] [INCREMENT BY n] [MINVALUE n] [MAXVALUE n] [CACHE n]
// [NO] [CYCLE];". Empty-string numeric fields are omitted.
struct SequenceOptions {
    std::int64_t start = 0;      // 0 = not set
    std::int64_t increment = 0;  // 0 = not set
    std::int64_t min_value = 0;  // 0 = not set
    std::int64_t max_value = 0;  // 0 = not set
    std::int64_t cache = 0;      // 0 = not set
    bool cycle = false;
    bool has_start = false;
    bool has_increment = false;
    bool has_min = false;
    bool has_max = false;
    bool has_cache = false;
};
std::string BuildCreateSequenceStatement(const std::string& seq_name, const SequenceOptions& opts);
std::string BuildDropSequenceStatement(const std::string& seq_name);

// BuildCreateViewStatement — emit "CREATE VIEW <name> AS <definition>;".
// `definition` is the SELECT text (e.g., "SELECT * FROM t WHERE x > 0").
std::string BuildCreateViewStatement(const std::string& view_name, const std::string& definition);
std::string BuildDropViewStatement(const std::string& view_name);

// BuildGrantStatement — emit "GRANT <privileges> ON <table> TO <role>;". An
// empty role (matching PG's PUBLIC) is rendered as the keyword PUBLIC.
std::string BuildGrantStatement(const std::string& privileges, const std::string& table_name,
                                const std::string& role);

// QuoteIdentifier — double-quote an identifier (doubling embedded quotes).
std::string QuoteIdentifier(const std::string& s);

// QuoteLiteral — single-quote a string literal (doubling embedded quotes).
std::string QuoteLiteral(const std::string& s);

}  // namespace pgcpp::tools

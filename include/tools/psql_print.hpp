// psql_print.h — Query-result output formatting (print.c).
//
// Converted from PostgreSQL 15's src/bin/psql/print.c.
//
// PG's print.c supports many output formats for query results: aligned
// (default), unaligned, HTML, LaTeX, CSV, JSON, etc. The format is selected
// by the `\pset format <name>` meta-command.
//
// pgcpp provides a faithful subset:
//   - kAligned:    column headers + ASCII-aligned columns + "(N rows)"
//   - kUnaligned:  pipe-separated values, no headers
//   - kCsv:        RFC 4180 CSV with quoting
//   - kJson:       JSON array of objects
//   - kHtml:       minimal HTML table
//   - kLatex:      minimal LaTeX tabular
//
// The PrintQueryResult function takes a QueryResult and a PrintOptions and
// writes the formatted output to a std::ostream.
#pragma once

#include <cstdint>
#include <ostream>
#include <string>

#include "tools/psql_client.hpp"

namespace pgcpp::tools {

// OutputFormat — the available `\pset format` values.
enum class OutputFormat {
    kAligned,
    kUnaligned,
    kCsv,
    kJson,
    kHtml,
    kLatex,
};

// PrintOptions — controls how a QueryResult is rendered.
struct PrintOptions {
    OutputFormat format = OutputFormat::kAligned;
    // Field separator for unaligned format (default "|").
    std::string field_sep = "|";
    // Record separator for unaligned format (default "\n").
    std::string record_sep = "\n";
    // Whether to print column headers (default true; ignored for CSV/JSON/HTML).
    bool show_headers = true;
    // Whether to print the "(N rows)" footer (default true; aligned only).
    bool show_footer = true;
    // Whether to expand output vertically (\x on) — one column per line.
    bool expanded = false;
    // Maximum column width before truncation (0 = no limit).
    int max_width = 0;
};

// ParseFormatName — convert a `\pset format` name to enum.
// Returns true on success.
bool ParseFormatName(const std::string& name, OutputFormat& out);

// FormatName — return the `\pset format` name for a given format.
std::string FormatName(OutputFormat format);

// PrintQueryResult — render `result` to `out` using `opts`.
// Returns the number of rows printed.
int PrintQueryResult(const QueryResult& result, const PrintOptions& opts, std::ostream& out);

// --- Per-format renderers (exposed for testing) ---

void PrintAligned(const QueryResult& result, const PrintOptions& opts, std::ostream& out);
void PrintUnaligned(const QueryResult& result, const PrintOptions& opts, std::ostream& out);
void PrintCsv(const QueryResult& result, std::ostream& out);
void PrintJson(const QueryResult& result, std::ostream& out);
void PrintHtml(const QueryResult& result, std::ostream& out);
void PrintLatex(const QueryResult& result, std::ostream& out);

// EscapeCsvField — quote a field per RFC 4180 if it contains comma, quote,
// newline, or CR.
std::string EscapeCsvField(const std::string& s);

// EscapeHtml — replace &, <, >, ", ' with HTML entities.
std::string EscapeHtml(const std::string& s);

// EscapeLatex — replace special LaTeX characters.
std::string EscapeLatex(const std::string& s);

// EscapeJson — escape a string for inclusion in a JSON string literal.
std::string EscapeJson(const std::string& s);

// TruncateCell — truncate `s` to `max_width` columns, appending "..." if
// truncated. Returns `s` unchanged if max_width == 0.
std::string TruncateCell(const std::string& s, int max_width);

}  // namespace pgcpp::tools

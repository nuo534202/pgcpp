// psql_command.h — Backslash meta-command dispatcher for psql.
//
// Converted from PostgreSQL 15's src/bin/psql/command.c::exec_command.
//
// ExecuteMetaCommand parses a single backslash-command line (e.g. "\\dt
// foo") and dispatches it. Recognized commands:
//
//   \\q, \\quit              — request to quit interactive loop
//   \\?, \\help              — print help
//   \\d [NAME]               — describe relation (or list tables when bare)
//   \\dt [PATTERN]           — list tables
//   \\dv [PATTERN]           — list views
//   \\l                      — list databases
//   \\du                     — list roles
//   \\c DBNAME               — connect to database (stub: prints message)
//   \\echo TEXT              — echo text (supports :var substitution)
//   \\i FILE                 — execute SQL from file
//   \\set [VAR VAL]          — set/list psql variables
//   \\unset VAR              — unset psql variable
//
// Output is written to the caller-supplied `out` stream, so the dispatcher
// can be unit-tested by passing a std::ostringstream.
#pragma once

#include <iosfwd>
#include <map>
#include <string>

namespace mytoydb::tools {

class PsqlClient;

// MetaCommandResult — outcome of dispatching a backslash command.
enum class MetaCommandResult {
    kQuit,      // \q — caller should exit the interactive loop
    kContinue,  // command handled (or unknown) — caller should keep reading
};

// ExecuteMetaCommand — parse and execute a single backslash command line.
//
// `line` is the raw user input starting with '\\' (whitespace tolerated).
// `vars` is the psql variable map (modified by \set / \unset).
// `out` receives human-readable output (help text, query results, errors).
//
// Returns kQuit for \\q/\\quit, kContinue for everything else.
MetaCommandResult ExecuteMetaCommand(PsqlClient& client, const std::string& line,
                                     std::map<std::string, std::string>& vars, std::ostream& out);

}  // namespace mytoydb::tools

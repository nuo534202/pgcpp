// psql_completion.h — Tab-completion for psql (tab.c).
//
// Converted from PostgreSQL 15's src/bin/psql/tab-complete.c.
//
// PG's tab-complete.c is a 4000-line state machine that completes SQL
// keywords, table names, column names, function names, etc. It uses readline
// / libedit for the actual line-editing UI.
//
// pgcpp does not link readline; the completion logic is exposed as a pure
// function that takes the current line + cursor position and returns the
// list of candidate completions. The CLI can wire this to any line editor.
//
// The implementation covers:
//   - SQL keyword completion (SELECT, FROM, WHERE, INSERT, UPDATE, DELETE, ...)
//   - Backslash meta-command completion (\dt, \dv, \l, \du, \d, \q, ...)
//   - Psql \pset subcommand completion (format, border, expanded, ...)
//   - Context-aware completion (e.g. after "ORDER BY" suggest column names
//     from the QueryResult's column_names)
#pragma once

#include <string>
#include <vector>

namespace pgcpp::tools {

// CompletionContext — inputs to the completion function.
struct CompletionContext {
    // The full input line (without trailing newline).
    std::string line;
    // The cursor position within `line` (0-based, in bytes).
    int cursor = 0;
    // The set of known table names (for FROM/INSERT/UPDATE completion).
    std::vector<std::string> table_names;
    // The set of known column names (for SELECT/WHERE/ORDER BY completion).
    std::vector<std::string> column_names;
};

// CompletionResult — the candidate completions for the current cursor.
struct CompletionResult {
    // The list of candidate strings (each is a full completion of the word
    // at the cursor; the caller picks one or displays them).
    std::vector<std::string> candidates;
    // The common prefix shared by all candidates (the longest unambiguous
    // completion). Empty if there is no common prefix beyond what's typed.
    std::string common_prefix;
};

// CompleteLine — compute the candidate completions for the given context.
CompletionResult CompleteLine(const CompletionContext& ctx);

// --- Sub-routines (exposed for testing) ---

// CompleteSqlCommand — complete a SQL keyword or identifier.
CompletionResult CompleteSqlCommand(const CompletionContext& ctx);

// CompleteMetaCommand — complete a backslash meta-command.
CompletionResult CompleteMetaCommand(const std::string& word);

// CompletePsetOption — complete a `\pset <option>` argument.
CompletionResult CompletePsetOption(const std::string& word);

// CommonPrefix — return the longest common prefix of all strings in `words`.
// Returns empty if `words` is empty.
std::string CommonPrefix(const std::vector<std::string>& words);

// SqlKeywords — the list of SQL keywords psql knows about (for completion).
const std::vector<std::string>& SqlKeywords();

// MetaCommands — the list of backslash commands psql knows about.
const std::vector<std::string>& MetaCommands();

}  // namespace pgcpp::tools

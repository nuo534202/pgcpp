// psql_variables.h — psql variable substitution (variables.c).
//
// Converted from PostgreSQL 15's src/bin/psql/variables.c.
//
// psql maintains a map of string variables set by the user via `\set NAME
// VALUE`. Variables can be referenced in SQL and meta-commands using
// `:NAME` (the value is substituted verbatim) or `:'NAME'` (the value is
// quoted as a SQL string literal).
//
// A small set of built-in "special" variables are also maintained:
//   - `AUTOCOMMIT`     — "on"/"off" (default on)
//   - `PROMPT1`/`PROMPT2`/`PROMPT3` — prompt templates
//   - `DBNAME`/`USER`/`HOST`/`PORT` — connection info
//   - `ENCODING`       — current client encoding
//   - `LASTOID`        — last OID returned by INSERT
//   - `ROW_COUNT`      — rows affected by the last command
//   - `SQLSTATE`       — SQLSTATE of the last command (empty if success)
#pragma once

#include <map>
#include <string>

namespace pgcpp::tools {

// PsqlVariables — the psql variable map.
class PsqlVariables {
public:
    PsqlVariables();

    // Set — assign `value` to `name`. Special variables are normalised.
    void Set(const std::string& name, const std::string& value);

    // Unset — remove `name` from the map (no-op if absent).
    void Unset(const std::string& name);

    // Get — return the value of `name`, or empty if not set.
    std::string Get(const std::string& name) const;

    // IsSet — true if `name` is set.
    bool IsSet(const std::string& name) const;

    // All — return a const reference to the underlying map (for iteration).
    const std::map<std::string, std::string>& All() const { return vars_; }

private:
    std::map<std::string, std::string> vars_;
};

// SubstituteVariables — replace `:NAME` and `:'NAME'` references in `text`
// using the values in `vars`. Unknown variables are left as-is (matching PG
// behaviour, which emits a warning).
//
// `:'NAME'` form quotes the value as a SQL string literal (doubling single
// quotes inside the value).
std::string SubstituteVariables(const std::string& text, const PsqlVariables& vars);

// QuoteSqlString — quote `s` as a SQL string literal (single-quoted, with
// embedded single quotes doubled). E.g. "it's" -> "'it''s'".
std::string QuoteSqlString(const std::string& s);

// ParseSetCommand — parse a `\set NAME VALUE` argument string into name and
// value. The value is the remainder of the line after the name (whitespace-
// trimmed). Returns false if no name is present.
bool ParseSetCommand(const std::string& args, std::string& name, std::string& value);

}  // namespace pgcpp::tools

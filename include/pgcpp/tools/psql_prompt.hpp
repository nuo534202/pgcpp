// psql_prompt.h — Prompt formatting (prompt.c).
//
// Converted from PostgreSQL 15's src/bin/psql/prompt.c.
//
// PG's prompt is a template string interpreted by get_prompt(). The template
// can contain `%`-escapes that are substituted with runtime state:
//   %/ — current database name
//   %~ — like %/ but "~" if the default database
//   %# — "#" if superuser, ">" otherwise
//   %n — current user name
//   %m — host name (short)
//   %> — remote port
//   %R — `=` in prompt1, `^` in prompt2 (after continuation), `!` if single-
//        user mode
//   %? — exit status of the last command (numeric)
//   `:VAR` — substitute a psql variable
//   `:'VAR'` — substitute a psql variable quoted as a SQL string
#pragma once

#include <string>

#include "pgcpp/tools/psql_variables.hpp"

namespace pgcpp::tools {

// PromptKind — which of the three PG prompts to format.
enum class PromptKind {
    kPrompt1,  // default prompt
    kPrompt2,  // continuation prompt (after a partial statement)
    kPrompt3,  // prompt for COPY ... FROM STDIN
};

// PromptContext — runtime state used to expand `%`-escapes.
struct PromptContext {
    std::string database;  // current database name
    std::string user;      // current user name
    std::string host;      // server host name
    int port = 0;          // server port
    bool is_superuser = false;
    int last_status = 0;  // exit status of the last command
    PromptKind kind = PromptKind::kPrompt1;
};

// FormatPrompt — expand the prompt template `tmpl` using `ctx` and `vars`.
// `tmpl` is typically the value of the PROMPT1/PROMPT2/PROMPT3 psql variable.
std::string FormatPrompt(const std::string& tmpl, const PromptContext& ctx,
                         const PsqlVariables& vars);

// DefaultPrompt — return the default template for the given prompt kind.
std::string DefaultPrompt(PromptKind kind);

}  // namespace pgcpp::tools

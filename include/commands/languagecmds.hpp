// languagecmds.h — CREATE/DROP LANGUAGE and DO (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/proclang.c.
//
// Implements the PL language framework's user-facing commands:
//   * CREATE [TRUSTED] [PROCEDURAL] LANGUAGE name
//       [HANDLER fn] [INLINE fn] [VALIDATOR fn]
//   * DROP LANGUAGE [IF EXISTS]
//   * DO [LANGUAGE lang] 'code'  — anonymous code block execution
#pragma once

#include <string>

namespace pgcpp::parser {
class CreateLanguageStmt;
class DropLanguageStmt;
class DoStmt;
}  // namespace pgcpp::parser

namespace pgcpp::commands {

// CreateLanguage — execute CREATE LANGUAGE.
// Inserts a row into pg_language. Returns the command tag.
std::string CreateLanguage(parser::CreateLanguageStmt* stmt);

// DropLanguage — execute DROP LANGUAGE.
// Removes the row from pg_language. Returns the command tag.
std::string DropLanguage(parser::DropLanguageStmt* stmt);

// DoBlock — execute DO [LANGUAGE lang] 'code'.
// Looks up the named language's inline handler and invokes it with the code.
// Returns the command tag.
std::string DoBlock(parser::DoStmt* stmt);

}  // namespace pgcpp::commands

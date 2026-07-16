// extensioncmds.h — CREATE/DROP EXTENSION (P3-10 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/extension.c.
//
// Implements the user-facing commands for the extension mechanism:
//   * CREATE EXTENSION [IF NOT EXISTS] name [WITH] [SCHEMA schema]
//       [VERSION version] [CASCADE]
//   * DROP EXTENSION [IF EXISTS] name [, ...] [CASCADE|RESTRICT]
//
// CREATE EXTENSION reads the control file from the extension registry,
// resolves the version (default_version if not specified), executes the
// extension's SQL script (if any), and inserts a pg_extension row.
// CASCADE recursively installs required extensions.
//
// DROP EXTENSION removes the pg_extension row. CASCADE support for dropping
// dependent objects is simplified (only the pg_extension row is removed).
#pragma once

#include <string>

namespace pgcpp::parser {
class CreateExtensionStmt;
class DropExtensionStmt;
}  // namespace pgcpp::parser

namespace pgcpp::commands {

// CreateExtension — execute CREATE EXTENSION.
// Looks up the control file, resolves the version, runs the SQL script,
// and inserts a pg_extension row. Returns the command tag.
std::string CreateExtension(parser::CreateExtensionStmt* stmt);

// DropExtension — execute DROP EXTENSION.
// Removes the pg_extension row(s). Returns the command tag.
std::string DropExtension(parser::DropExtensionStmt* stmt);

}  // namespace pgcpp::commands

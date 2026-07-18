// systemcmds.h — ALTER SYSTEM (P3-13 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/variable.c
// (the ALTER SYSTEM SET/RESET portion).
//
// ALTER SYSTEM modifies persistent GUC configuration in postgresql.auto.conf
// and signals the postmaster to reload. RESET ALL clears all auto.conf
// settings.
//
// pgcpp's skeleton implementation parses the statement and dispatches to
// this handler. Persistence to postgresql.auto.conf lands in a future
// phase; until then the handler validates the parse tree and returns the
// command tag.
#pragma once

#include <string>

namespace pgcpp::parser {
class AlterSystemStmt;
}  // namespace pgcpp::parser

namespace pgcpp::commands {

// AlterSystem — execute ALTER SYSTEM SET / RESET / RESET ALL.
std::string AlterSystem(parser::AlterSystemStmt* stmt);

}  // namespace pgcpp::commands

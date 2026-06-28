// typecmds.h — CREATE TYPE (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/typecmds.c.
#pragma once

#include <string>

namespace mytoydb::parser {
class CreateStmt;
}  // namespace mytoydb::parser

namespace mytoydb::commands {

// DefineType — execute CREATE TYPE. Stub (MyToyDB has no user-defined
// type system yet).
std::string DefineType(parser::CreateStmt* stmt);

}  // namespace mytoydb::commands

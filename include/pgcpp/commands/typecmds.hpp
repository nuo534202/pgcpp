// typecmds.h — CREATE TYPE (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/typecmds.c.
#pragma once

#include <string>

namespace pgcpp::parser {
class CreateStmt;
}  // namespace pgcpp::parser

namespace pgcpp::commands {

// DefineType — execute CREATE TYPE. Stub (pgcpp has no user-defined
// type system yet).
std::string DefineType(parser::CreateStmt* stmt);

}  // namespace pgcpp::commands

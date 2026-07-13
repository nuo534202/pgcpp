// typecmds.h — CREATE TYPE (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/typecmds.c.
#pragma once

#include <string>

namespace pgcpp::parser {
class CreateTypeStmt;
}  // namespace pgcpp::parser

namespace pgcpp::commands {

// DefineType — execute CREATE TYPE ... AS ENUM.
// Registers a pg_type row with typtype=kEnum and stores enum labels
// in the typdefault field as a comma-separated string.
std::string DefineType(parser::CreateTypeStmt* stmt);

}  // namespace pgcpp::commands

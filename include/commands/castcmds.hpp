// castcmds.h — CREATE CAST (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/aggregatecmds.c.
#pragma once

#include <string>

namespace pgcpp::parser {
class CreateCastStmt;
}  // namespace pgcpp::parser

namespace pgcpp::commands {

// CreateCast — execute CREATE CAST.
// Registers a pg_cast row with source/target type OIDs, cast function,
// context (explicit/assignment/implicit), and method (function/binary).
std::string CreateCast(parser::CreateCastStmt* stmt);

}  // namespace pgcpp::commands

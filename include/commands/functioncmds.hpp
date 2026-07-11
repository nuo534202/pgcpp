// functioncmds.h — CREATE FUNCTION (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/functioncmds.c.
#pragma once

#include <string>

namespace pgcpp::parser {
class CreateFunctionStmt;
}  // namespace pgcpp::parser

namespace pgcpp::commands {

// CreateFunction — execute CREATE FUNCTION.
// Parses options (LANGUAGE, AS, volatility, strict), builds a pg_proc row,
// and persists it via the Catalog.
std::string CreateFunction(parser::CreateFunctionStmt* stmt);

}  // namespace pgcpp::commands

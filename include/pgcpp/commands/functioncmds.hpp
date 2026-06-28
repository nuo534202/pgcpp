// functioncmds.h — CREATE FUNCTION (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/functioncmds.c.
#pragma once

#include <string>

namespace pgcpp::parser {
class CreateFunctionStmt;
}  // namespace pgcpp::parser

namespace pgcpp::commands {

// CreateFunction — execute CREATE FUNCTION. Stub.
std::string CreateFunction(parser::CreateFunctionStmt* stmt);

}  // namespace pgcpp::commands

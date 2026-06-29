// operatorcmds.h — CREATE OPERATOR (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/operatorcmds.c.
#pragma once

#include <string>

namespace pgcpp::parser {
class CreateStmt;
}  // namespace pgcpp::parser

namespace pgcpp::commands {

// DefineOperator — execute CREATE OPERATOR. Stub.
std::string DefineOperator(parser::CreateStmt* stmt);

}  // namespace pgcpp::commands

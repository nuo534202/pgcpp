// opclasscmds.h — CREATE OPERATOR CLASS (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/opclasscmds.c.
#pragma once

#include <string>

namespace pgcpp::parser {
class CreateStmt;
}  // namespace pgcpp::parser

namespace pgcpp::commands {

// DefineOpClass — execute CREATE OPERATOR CLASS. Stub.
std::string DefineOpClass(parser::CreateStmt* stmt);

}  // namespace pgcpp::commands

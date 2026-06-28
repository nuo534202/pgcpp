// operatorcmds.h — CREATE OPERATOR (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/operatorcmds.c.
#pragma once

#include <string>

namespace mytoydb::parser {
class CreateStmt;
}  // namespace mytoydb::parser

namespace mytoydb::commands {

// DefineOperator — execute CREATE OPERATOR. Stub.
std::string DefineOperator(parser::CreateStmt* stmt);

}  // namespace mytoydb::commands

// functioncmds.h — CREATE FUNCTION (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/functioncmds.c.
#pragma once

#include <string>

namespace mytoydb::parser {
class CreateFunctionStmt;
}  // namespace mytoydb::parser

namespace mytoydb::commands {

// CreateFunction — execute CREATE FUNCTION. Stub.
std::string CreateFunction(parser::CreateFunctionStmt* stmt);

}  // namespace mytoydb::commands

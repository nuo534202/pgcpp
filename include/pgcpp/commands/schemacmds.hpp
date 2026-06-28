// schemacmds.h — CREATE SCHEMA (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/schemacmds.c.
#pragma once

#include <string>

namespace pgcpp::parser {
class CreateSchemaStmt;
}  // namespace pgcpp::parser

namespace pgcpp::commands {

// CreateSchemaCommand — execute CREATE SCHEMA. Stub.
std::string CreateSchemaCommand(parser::CreateSchemaStmt* stmt);

}  // namespace pgcpp::commands

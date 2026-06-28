// schemacmds.h — CREATE SCHEMA (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/schemacmds.c.
#pragma once

#include <string>

namespace mytoydb::parser {
class CreateSchemaStmt;
}  // namespace mytoydb::parser

namespace mytoydb::commands {

// CreateSchemaCommand — execute CREATE SCHEMA. Stub.
std::string CreateSchemaCommand(parser::CreateSchemaStmt* stmt);

}  // namespace mytoydb::commands

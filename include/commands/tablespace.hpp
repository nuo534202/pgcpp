// tablespace.h — CREATE TABLESPACE (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/tablespace.c.
#pragma once

#include <string>

namespace pgcpp::parser {
class CreateTableSpaceStmt;
class DropTableSpaceStmt;
}  // namespace pgcpp::parser

namespace pgcpp::commands {

// CreateTableSpace — execute CREATE TABLESPACE. Stub.
std::string CreateTableSpace(parser::CreateTableSpaceStmt* stmt);

// DropTableSpace — execute DROP TABLESPACE. Stub.
std::string DropTableSpace(parser::DropTableSpaceStmt* stmt);

}  // namespace pgcpp::commands

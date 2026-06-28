// tablespace.h — CREATE TABLESPACE (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/tablespace.c.
#pragma once

#include <string>

namespace mytoydb::parser {
class CreateTableSpaceStmt;
class DropTableSpaceStmt;
}  // namespace mytoydb::parser

namespace mytoydb::commands {

// CreateTableSpace — execute CREATE TABLESPACE. Stub.
std::string CreateTableSpace(parser::CreateTableSpaceStmt* stmt);

// DropTableSpace — execute DROP TABLESPACE. Stub.
std::string DropTableSpace(parser::DropTableSpaceStmt* stmt);

}  // namespace mytoydb::commands

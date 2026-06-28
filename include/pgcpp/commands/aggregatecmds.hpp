// aggregatecmds.h — CREATE AGGREGATE (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/aggregatecmds.c.
#pragma once

#include <string>

namespace mytoydb::parser {
class CreateStmt;
}  // namespace mytoydb::parser

namespace mytoydb::commands {

// DefineAggregate — execute CREATE AGGREGATE. Stub.
std::string DefineAggregate(parser::CreateStmt* stmt);

}  // namespace mytoydb::commands

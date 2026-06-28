// aggregatecmds.h — CREATE AGGREGATE (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/aggregatecmds.c.
#pragma once

#include <string>

namespace pgcpp::parser {
class CreateStmt;
}  // namespace pgcpp::parser

namespace pgcpp::commands {

// DefineAggregate — execute CREATE AGGREGATE. Stub.
std::string DefineAggregate(parser::CreateStmt* stmt);

}  // namespace pgcpp::commands

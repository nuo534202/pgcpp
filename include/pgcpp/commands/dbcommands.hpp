// dbcommands.h — CREATE/DROP DATABASE (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/dbcommands.c.
#pragma once

#include <string>

namespace pgcpp::parser {
class CreatedbStmt;
class DropdbStmt;
}  // namespace pgcpp::parser

namespace pgcpp::commands {

// createdb — execute CREATE DATABASE. pgcpp is a single-database
// instance; this is a stub that acknowledges the request.
std::string createdb(parser::CreatedbStmt* stmt);

// dropdb — execute DROP DATABASE. Stub.
std::string dropdb(parser::DropdbStmt* stmt);

}  // namespace pgcpp::commands

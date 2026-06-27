// dbcommands.h — CREATE/DROP DATABASE (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/dbcommands.c.
#pragma once

#include <string>

namespace mytoydb::parser {
class CreatedbStmt;
class DropdbStmt;
}  // namespace mytoydb::parser

namespace mytoydb::commands {

// createdb — execute CREATE DATABASE. MyToyDB is a single-database
// instance; this is a stub that acknowledges the request.
std::string createdb(parser::CreatedbStmt* stmt);

// dropdb — execute DROP DATABASE. Stub.
std::string dropdb(parser::DropdbStmt* stmt);

}  // namespace mytoydb::commands

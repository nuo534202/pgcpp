// foreigncmds.h — CREATE FOREIGN TABLE / SERVER, ALTER SERVER, DROP SERVER,
// IMPORT FOREIGN SCHEMA (P3-13 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/foreigncmds.c.
//
// Foreign tables and servers are FDW (Foreign Data Wrapper) objects.
// CREATE SERVER registers a row in pg_foreign_server (referencing a
// pg_foreign_data_wrapper row). CREATE FOREIGN TABLE creates a relkind =
// 'f' relation (pg_class row) plus pg_foreign_table row. IMPORT FOREIGN
// SCHEMA delegates to the FDW's ImportForeignSchema handler.
//
// pgcpp's skeleton implementation parses the statements and dispatches to
// these handlers. Actual FDW execution (table scans via FDW routes) lands
// in P3-5 (FDW executor); until then the handlers validate the parse tree
// and return the command tag.
#pragma once

#include <string>

namespace pgcpp::parser {
class CreateForeignTableStmt;
class CreateServerStmt;
class AlterServerStmt;
class DropServerStmt;
class ImportForeignSchemaStmt;
}  // namespace pgcpp::parser

namespace pgcpp::commands {

// CreateForeignTable — execute CREATE FOREIGN TABLE.
std::string CreateForeignTable(parser::CreateForeignTableStmt* stmt);

// CreateServer — execute CREATE SERVER.
std::string CreateServer(parser::CreateServerStmt* stmt);

// AlterServer — execute ALTER SERVER.
std::string AlterServer(parser::AlterServerStmt* stmt);

// DropServer — execute DROP SERVER.
std::string DropServer(parser::DropServerStmt* stmt);

// ImportForeignSchema — execute IMPORT FOREIGN SCHEMA.
std::string ImportForeignSchema(parser::ImportForeignSchemaStmt* stmt);

}  // namespace pgcpp::commands

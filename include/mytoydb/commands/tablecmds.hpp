// tablecmds.h — CREATE/ALTER/DROP TABLE implementation (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/tablecmds.c.
//
// These functions are invoked by ProcessUtility (tcop/utility.c equivalent)
// for relation-related DDL: CREATE TABLE, DROP TABLE/INDEX/VIEW, ALTER TABLE
// (ADD/DROP/RENAME COLUMN), TRUNCATE TABLE.
#pragma once

#include <string>

namespace mytoydb::parser {
class CreateStmt;
class DropStmt;
class AlterTableStmt;
class RenameStmt;
class TruncateStmt;
}  // namespace mytoydb::parser

namespace mytoydb::commands {

// DefineRelation — CREATE TABLE. Creates pg_class + pg_attribute entries
// and physical storage. Returns the command tag ("CREATE TABLE").
std::string DefineRelation(parser::CreateStmt* stmt);

// RemoveRelations — DROP TABLE / DROP INDEX / DROP VIEW. Iterates the
// object list and drops each relation's storage and catalog entries.
std::string RemoveRelations(parser::DropStmt* stmt);

// AlterTable — ALTER TABLE ADD/DROP COLUMN (and other subcommands).
std::string AlterTable(parser::AlterTableStmt* stmt);

// RenameRelation — ALTER TABLE RENAME (column or relation).
std::string RenameRelation(parser::RenameStmt* stmt);

// ExecuteTruncate — TRUNCATE TABLE.
std::string ExecuteTruncate(parser::TruncateStmt* stmt);

}  // namespace mytoydb::commands

// publicationcmds.h — CREATE / ALTER / DROP PUBLICATION (P3-13 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/publicationcmds.c.
//
// Publications are logical-replication table sets. CREATE PUBLICATION
// registers a row in pg_publication (with optional pg_publication_rel rows
// for member tables). Subscribers consume the publication via a replication
// slot.
//
// pgcpp's skeleton implementation parses the statements and dispatches to
// these handlers. Actual replication wire-up lands in P3-4 (logical
// replication); until then the handlers validate the parse tree and return
// the command tag.
#pragma once

#include <string>

namespace pgcpp::parser {
class CreatePublicationStmt;
class AlterPublicationStmt;
class DropPublicationStmt;
}  // namespace pgcpp::parser

namespace pgcpp::commands {

// CreatePublication — execute CREATE PUBLICATION.
std::string CreatePublication(parser::CreatePublicationStmt* stmt);

// AlterPublication — execute ALTER PUBLICATION.
std::string AlterPublication(parser::AlterPublicationStmt* stmt);

// DropPublication — execute DROP PUBLICATION.
std::string DropPublication(parser::DropPublicationStmt* stmt);

}  // namespace pgcpp::commands

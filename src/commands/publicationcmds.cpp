// publicationcmds.cpp — CREATE / ALTER / DROP PUBLICATION implementation (P3-13).
//
// Converted from PostgreSQL 15's src/backend/commands/publicationcmds.c.
//
// Skeleton implementation: validates the parse tree and returns the command
// tag. Logical replication wire-up lands in P3-4.
#include "commands/publicationcmds.hpp"

#include <string>

#include "common/error/elog.hpp"
#include "parser/parsenodes.hpp"

namespace pgcpp::commands {

using pgcpp::parser::AlterPublicationStmt;
using pgcpp::parser::CreatePublicationStmt;
using pgcpp::parser::DropPublicationStmt;

std::string CreatePublication(CreatePublicationStmt* stmt) {
    if (stmt == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "CREATE PUBLICATION: null statement");
    }
    if (stmt->pubname.empty()) {
        ereport(pgcpp::error::LogLevel::kError, "CREATE PUBLICATION: missing publication name");
    }
    // pg_publication insert and pg_publication_rel rows land in P3-4.
    return "CREATE PUBLICATION";
}

std::string AlterPublication(AlterPublicationStmt* stmt) {
    if (stmt == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "ALTER PUBLICATION: null statement");
    }
    if (stmt->pubname.empty()) {
        ereport(pgcpp::error::LogLevel::kError, "ALTER PUBLICATION: missing publication name");
    }
    return "ALTER PUBLICATION";
}

std::string DropPublication(DropPublicationStmt* stmt) {
    if (stmt == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "DROP PUBLICATION: null statement");
    }
    if (stmt->pubnames.empty()) {
        ereport(pgcpp::error::LogLevel::kError, "DROP PUBLICATION: missing publication name");
    }
    // IF EXISTS handling and CASCADE behavior land with pg_publication persistence.
    return "DROP PUBLICATION";
}

}  // namespace pgcpp::commands

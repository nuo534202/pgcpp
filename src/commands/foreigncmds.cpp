// foreigncmds.cpp — CREATE FOREIGN TABLE / SERVER, ALTER SERVER, DROP SERVER,
// IMPORT FOREIGN SCHEMA implementation (P3-13).
//
// Converted from PostgreSQL 15's src/backend/commands/foreigncmds.c.
//
// Skeleton implementation: validates the parse tree and returns the command
// tag. FDW execution (foreign scans via FDW routes) lands in P3-5.
#include "commands/foreigncmds.hpp"

#include <string>

#include "common/error/elog.hpp"
#include "parser/parsenodes.hpp"

namespace pgcpp::commands {

using pgcpp::parser::AlterServerStmt;
using pgcpp::parser::CreateForeignTableStmt;
using pgcpp::parser::CreateServerStmt;
using pgcpp::parser::DropServerStmt;
using pgcpp::parser::ImportForeignSchemaStmt;

std::string CreateForeignTable(CreateForeignTableStmt* stmt) {
    if (stmt == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "CREATE FOREIGN TABLE: null statement");
    }
    if (stmt->relation == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "CREATE FOREIGN TABLE: missing relation name");
    }
    if (stmt->servername.empty()) {
        ereport(pgcpp::error::LogLevel::kError, "CREATE FOREIGN TABLE: missing SERVER name");
    }
    // pg_class + pg_foreign_table inserts and FDW route setup land in P3-5.
    return "CREATE FOREIGN TABLE";
}

std::string CreateServer(CreateServerStmt* stmt) {
    if (stmt == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "CREATE SERVER: null statement");
    }
    if (stmt->servername.empty()) {
        ereport(pgcpp::error::LogLevel::kError, "CREATE SERVER: missing server name");
    }
    if (stmt->fdwname.empty()) {
        ereport(pgcpp::error::LogLevel::kError, "CREATE SERVER: missing FOREIGN DATA WRAPPER name");
    }
    // pg_foreign_server insert lands with FDW support in P3-5.
    return "CREATE SERVER";
}

std::string AlterServer(AlterServerStmt* stmt) {
    if (stmt == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "ALTER SERVER: null statement");
    }
    if (stmt->servername.empty()) {
        ereport(pgcpp::error::LogLevel::kError, "ALTER SERVER: missing server name");
    }
    return "ALTER SERVER";
}

std::string DropServer(DropServerStmt* stmt) {
    if (stmt == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "DROP SERVER: null statement");
    }
    if (stmt->servernames.empty()) {
        ereport(pgcpp::error::LogLevel::kError, "DROP SERVER: missing server name");
    }
    return "DROP SERVER";
}

std::string ImportForeignSchema(ImportForeignSchemaStmt* stmt) {
    if (stmt == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "IMPORT FOREIGN SCHEMA: null statement");
    }
    if (stmt->remote_schema.empty()) {
        ereport(pgcpp::error::LogLevel::kError,
                "IMPORT FOREIGN SCHEMA: missing remote schema name");
    }
    if (stmt->server_name.empty()) {
        ereport(pgcpp::error::LogLevel::kError, "IMPORT FOREIGN SCHEMA: missing SERVER name");
    }
    if (stmt->local_schema.empty()) {
        ereport(pgcpp::error::LogLevel::kError, "IMPORT FOREIGN SCHEMA: missing INTO schema name");
    }
    // FDW ImportForeignSchema callback invocation lands in P3-5.
    return "IMPORT FOREIGN SCHEMA";
}

}  // namespace pgcpp::commands

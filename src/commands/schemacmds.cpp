// schemacmds.cpp — CREATE SCHEMA implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/schemacmds.c.
// MyToyDB currently uses a single "public" namespace (OID 2200); CREATE
// SCHEMA is a stub that acknowledges the request.
#include "mytoydb/commands/schemacmds.hpp"

#include <string>

#include "mytoydb/parser/parsenodes.hpp"

namespace mytoydb::commands {

using mytoydb::parser::CreateSchemaStmt;

std::string CreateSchemaCommand(CreateSchemaStmt* stmt) {
    if (stmt == nullptr)
        return "CREATE SCHEMA";
    // Stub: real PostgreSQL creates a pg_namespace row and processes
    // schema_elts (CREATE TABLE / CREATE VIEW / GRANT within the schema).
    return "CREATE SCHEMA";
}

}  // namespace mytoydb::commands

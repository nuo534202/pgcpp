// tablespace.cpp — CREATE/DROP TABLESPACE implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/tablespace.c.
// MyToyDB uses a single storage directory; this is a stub.
#include "pgcpp/commands/tablespace.hpp"

#include <string>

#include "pgcpp/parser/parsenodes.hpp"

namespace mytoydb::commands {

using mytoydb::parser::CreateTableSpaceStmt;
using mytoydb::parser::DropTableSpaceStmt;

std::string CreateTableSpace(CreateTableSpaceStmt* stmt) {
    (void)stmt;  // Stub.
    return "CREATE TABLESPACE";
}

std::string DropTableSpace(DropTableSpaceStmt* stmt) {
    (void)stmt;  // Stub.
    return "DROP TABLESPACE";
}

}  // namespace mytoydb::commands

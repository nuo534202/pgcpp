// indexcmds.h — CREATE/ALTER/DROP INDEX implementation (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/indexcmds.c.
#pragma once

#include <string>

namespace mytoydb::parser {
class IndexStmt;
}  // namespace mytoydb::parser

namespace mytoydb::commands {

// DefineIndex — CREATE INDEX. Creates pg_class + pg_attribute entries for
// the index relation and builds the B-tree on the heap.
std::string DefineIndex(parser::IndexStmt* stmt);

}  // namespace mytoydb::commands

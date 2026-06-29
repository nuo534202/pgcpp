// copy.h — COPY command router (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/copy.c. The actual
// row-reading (COPY FROM) and row-writing (COPY TO) live in copy_from.cpp
// and copy_to.cpp respectively.
#pragma once

#include <string>

namespace pgcpp::parser {
class CopyStmt;
}  // namespace pgcpp::parser

namespace pgcpp::commands {

// DoCopy — top-level COPY dispatcher. Routes to CopyFrom or CopyTo based
// on stmt->is_from. Returns the command tag ("COPY <n>").
std::string DoCopy(parser::CopyStmt* stmt);

}  // namespace pgcpp::commands

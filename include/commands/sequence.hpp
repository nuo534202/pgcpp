// sequence.h — CREATE SEQUENCE (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/sequence.c.
#pragma once

#include <string>

namespace pgcpp::parser {
class CreateSeqStmt;
}  // namespace pgcpp::parser

namespace pgcpp::commands {

// DefineSequence — execute CREATE SEQUENCE. Creates a relation of relkind
// 'S' (sequence). Returns "CREATE SEQUENCE".
std::string DefineSequence(parser::CreateSeqStmt* stmt);

}  // namespace pgcpp::commands

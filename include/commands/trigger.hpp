// trigger.h — CREATE TRIGGER (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/trigger.c.
#pragma once

#include <string>

namespace pgcpp::parser {
class CreateTrigStmt;
}  // namespace pgcpp::parser

namespace pgcpp::commands {

// CreateTrigger — execute CREATE TRIGGER. pgcpp doesn't execute
// triggers at runtime yet; this records the trigger definition so that
// \d and dump can report it. Returns "CREATE TRIGGER".
std::string CreateTrigger(parser::CreateTrigStmt* stmt);

}  // namespace pgcpp::commands

// trigger.h — CREATE TRIGGER (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/trigger.c.
#pragma once

#include <string>

namespace mytoydb::parser {
class CreateTrigStmt;
}  // namespace mytoydb::parser

namespace mytoydb::commands {

// CreateTrigger — execute CREATE TRIGGER. MyToyDB doesn't execute
// triggers at runtime yet; this records the trigger definition so that
// \d and dump can report it. Returns "CREATE TRIGGER".
std::string CreateTrigger(parser::CreateTrigStmt* stmt);

}  // namespace mytoydb::commands

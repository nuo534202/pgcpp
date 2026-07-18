// seclabelcmds.h — SECURITY LABEL (P3-13 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/seclabel.c.
//
// SECURITY LABEL attaches a provider-specific label to a database object.
// The label is stored in pg_seclabel (or pg_shseclabel for shared objects).
// Providers (e.g., SELinux) register themselves via a C hook.
//
// pgcpp's skeleton implementation parses the statement and dispatches to
// this handler. Provider hooks and persistence land in a future phase;
// until then the handler validates the parse tree and returns the
// command tag.
#pragma once

#include <string>

namespace pgcpp::parser {
class SecLabelStmt;
}  // namespace pgcpp::parser

namespace pgcpp::commands {

// SecLabel — execute SECURITY LABEL.
std::string SecLabel(parser::SecLabelStmt* stmt);

}  // namespace pgcpp::commands

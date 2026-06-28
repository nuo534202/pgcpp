// view.h — CREATE VIEW (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/view.c.
#pragma once

#include <string>

namespace pgcpp::parser {
class ViewStmt;
}  // namespace pgcpp::parser

namespace pgcpp::commands {

// DefineView — execute CREATE VIEW. Stores the view's query in pg_class
// (relkind 'v') without materializing data. Returns "CREATE VIEW".
std::string DefineView(parser::ViewStmt* stmt);

}  // namespace pgcpp::commands

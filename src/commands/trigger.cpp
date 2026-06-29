// trigger.cpp — CREATE TRIGGER implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/trigger.c.
// pgcpp doesn't execute triggers at runtime yet; this stub records
// the trigger definition by setting relhastriggers on the target
// relation's pg_class entry so \d and dump can report it.
#include "commands/trigger.hpp"

#include <string>

#include "catalog/catalog.hpp"
#include "catalog/pg_class.hpp"
#include "common/error/elog.hpp"
#include "parser/parsenodes.hpp"

namespace pgcpp::commands {

using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_class;
using pgcpp::catalog::GetCatalog;
using pgcpp::parser::CreateTrigStmt;

std::string CreateTrigger(CreateTrigStmt* stmt) {
    if (stmt == nullptr || stmt->relation == nullptr)
        return "";

    Catalog* cat = GetCatalog();
    if (cat == nullptr)
        return "";

    const std::string& relname = stmt->relation->relname;
    const FormData_pg_class* class_row = cat->GetClassByName(relname);
    if (class_row == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "relation \"" + relname + "\" does not exist");
    }

    // Mark the relation as having triggers. The full trigger definition
    // (pg_trigger rows) is not yet persisted — that's a future task.
    auto* mut = const_cast<FormData_pg_class*>(class_row);
    mut->relhastriggers = true;

    return "CREATE TRIGGER";
}

}  // namespace pgcpp::commands

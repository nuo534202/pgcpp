// trigger.cpp — CREATE TRIGGER implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/trigger.c.
// MyToyDB doesn't execute triggers at runtime yet; this stub records
// the trigger definition by setting relhastriggers on the target
// relation's pg_class entry so \d and dump can report it.
#include "mytoydb/commands/trigger.hpp"

#include <string>

#include "mytoydb/catalog/catalog.hpp"
#include "mytoydb/catalog/pg_class.hpp"
#include "mytoydb/common/error/elog.hpp"
#include "mytoydb/parser/parsenodes.hpp"

namespace mytoydb::commands {

using mytoydb::catalog::Catalog;
using mytoydb::catalog::FormData_pg_class;
using mytoydb::catalog::GetCatalog;
using mytoydb::parser::CreateTrigStmt;

std::string CreateTrigger(CreateTrigStmt* stmt) {
    if (stmt == nullptr || stmt->relation == nullptr)
        return "";

    Catalog* cat = GetCatalog();
    if (cat == nullptr)
        return "";

    const std::string& relname = stmt->relation->relname;
    const FormData_pg_class* class_row = cat->GetClassByName(relname);
    if (class_row == nullptr) {
        ereport(mytoydb::error::LogLevel::kError, "relation \"" + relname + "\" does not exist");
    }

    // Mark the relation as having triggers. The full trigger definition
    // (pg_trigger rows) is not yet persisted — that's a future task.
    auto* mut = const_cast<FormData_pg_class*>(class_row);
    mut->relhastriggers = true;

    return "CREATE TRIGGER";
}

}  // namespace mytoydb::commands

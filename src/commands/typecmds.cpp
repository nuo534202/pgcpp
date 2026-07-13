// typecmds.cpp — CREATE TYPE implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/typecmds.c.
// Currently supports CREATE TYPE ... AS ENUM only.
#include "commands/typecmds.hpp"

#include <string>

#include "catalog/catalog.hpp"
#include "catalog/pg_type.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "parser/parsenodes.hpp"

namespace pgcpp::commands {

using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_type;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::TypeCategory;
using pgcpp::catalog::TypeType;
using pgcpp::nodes::makePallocNode;
using pgcpp::nodes::Value;
using pgcpp::parser::CreateTypeStmt;

// Extract the type name from the list of String nodes.
static std::string GetTypeName(const std::vector<pgcpp::nodes::Node*>& name_parts) {
    std::string result;
    for (const auto* node : name_parts) {
        const auto* v = dynamic_cast<const Value*>(node);
        if (v != nullptr) {
            if (!result.empty())
                result += ".";
            result += v->GetString();
        }
    }
    return result;
}

std::string DefineType(CreateTypeStmt* stmt) {
    if (stmt == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "CREATE TYPE: null statement");
    }

    std::string type_name = GetTypeName(stmt->type_name);
    if (type_name.empty()) {
        ereport(pgcpp::error::LogLevel::kError, "CREATE TYPE: missing type name");
    }

    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "CREATE TYPE: catalog not initialized");
    }

    // Check for duplicate type name.
    if (cat->GetTypeByName(type_name) != nullptr) {
        ereport(pgcpp::error::LogLevel::kError,
                "CREATE TYPE: type \"" + type_name + "\" already exists");
    }

    // Build the pg_type row for an enum type.
    auto* type_row = makePallocNode<FormData_pg_type>();
    type_row->oid = pgcpp::catalog::kInvalidOid;  // assigned by InsertType
    type_row->typname = type_name;
    type_row->typlen = 4;  // enums are stored as int4 internally
    type_row->typbyval = true;
    type_row->typtype = TypeType::kEnum;
    type_row->typcategory = TypeCategory::kEnum;
    type_row->typispreferred = false;
    type_row->typisdefined = true;
    type_row->typalign = pgcpp::catalog::TypeAlign::kInt;
    type_row->typstorage = pgcpp::catalog::TypeStorage::kPlain;

    // Store enum labels as a comma-separated string in typdefault.
    // A full implementation would use a separate pg_enum catalog table.
    std::string labels_str;
    for (size_t i = 0; i < stmt->labels.size(); ++i) {
        if (i > 0)
            labels_str += ",";
        labels_str += stmt->labels[i];
    }
    type_row->typdefault = labels_str;

    cat->InsertType(type_row);

    return "CREATE TYPE";
}

}  // namespace pgcpp::commands

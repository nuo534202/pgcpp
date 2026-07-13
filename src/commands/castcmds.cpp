// castcmds.cpp — CREATE CAST implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/aggregatecmds.c.
#include "commands/castcmds.hpp"

#include <string>

#include "catalog/catalog.hpp"
#include "catalog/pg_cast.hpp"
#include "catalog/pg_type.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "parser/parsenodes.hpp"

namespace pgcpp::commands {

using pgcpp::catalog::CastContext;
using pgcpp::catalog::CastMethod;
using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_cast;
using pgcpp::catalog::FormData_pg_type;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::kInvalidOid;
using pgcpp::nodes::makePallocNode;
using pgcpp::nodes::Value;
using pgcpp::parser::CreateCastStmt;
using pgcpp::parser::TypeName;

// Extract a type name string from a TypeName node (last component only).
static std::string TypeNameToString(const TypeName* tn) {
    if (tn == nullptr)
        return "";
    std::string result;
    for (const auto* node : tn->names) {
        const auto* v = dynamic_cast<const Value*>(node);
        if (v != nullptr)
            result = v->GetString();
    }
    return result;
}

std::string CreateCast(CreateCastStmt* stmt) {
    if (stmt == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "CREATE CAST: null statement");
    }

    std::string source_name = TypeNameToString(stmt->sourcetype);
    std::string target_name = TypeNameToString(stmt->targettype);

    if (source_name.empty() || target_name.empty()) {
        ereport(pgcpp::error::LogLevel::kError, "CREATE CAST: missing source or target type");
    }

    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "CREATE CAST: catalog not initialized");
    }

    // Resolve source and target type OIDs.
    const FormData_pg_type* source_type = cat->GetTypeByName(source_name);
    if (source_type == nullptr) {
        ereport(pgcpp::error::LogLevel::kError,
                "CREATE CAST: source type \"" + source_name + "\" does not exist");
    }

    const FormData_pg_type* target_type = cat->GetTypeByName(target_name);
    if (target_type == nullptr) {
        ereport(pgcpp::error::LogLevel::kError,
                "CREATE CAST: target type \"" + target_name + "\" does not exist");
    }

    // Check for duplicate cast.
    if (cat->GetCast(source_type->oid, target_type->oid) != nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "CREATE CAST: cast from \"" + source_name +
                                                    "\" to \"" + target_name + "\" already exists");
    }

    // Build the pg_cast row.
    auto* cast_row = makePallocNode<FormData_pg_cast>();
    cast_row->castsource = source_type->oid;
    cast_row->casttarget = target_type->oid;
    cast_row->castcontext = stmt->context;

    if (stmt->without_function) {
        cast_row->castfunc = kInvalidOid;
        cast_row->castmethod = CastMethod::kBinary;
    } else {
        // For WITH FUNCTION, we don't resolve the function OID here
        // (would require fmgr lookup). Store as kInvalidOid for now.
        cast_row->castfunc = kInvalidOid;
        cast_row->castmethod = CastMethod::kFunction;
    }

    cat->InsertCast(cast_row);

    return "CREATE CAST";
}

}  // namespace pgcpp::commands

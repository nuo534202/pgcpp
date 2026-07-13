// domaincmds.cpp — CREATE DOMAIN implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/domaincmds.c.
#include "commands/domaincmds.hpp"

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
using pgcpp::catalog::kInvalidOid;
using pgcpp::catalog::TypeCategory;
using pgcpp::catalog::TypeType;
using pgcpp::nodes::makePallocNode;
using pgcpp::nodes::Value;
using pgcpp::parser::CreateDomainStmt;
using pgcpp::parser::TypeName;

// Extract a type name string from a TypeName node (last component only,
// matching PostgreSQL's search_path-based type lookup).
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

// Extract the domain name from the list of String nodes.
static std::string GetDomainName(const std::vector<pgcpp::nodes::Node*>& name_parts) {
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

std::string DefineDomain(CreateDomainStmt* stmt) {
    if (stmt == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "CREATE DOMAIN: null statement");
    }

    std::string domain_name = GetDomainName(stmt->domainname);
    if (domain_name.empty()) {
        ereport(pgcpp::error::LogLevel::kError, "CREATE DOMAIN: missing domain name");
    }

    // Resolve the base type.
    std::string base_type_name = TypeNameToString(stmt->type_name);
    if (base_type_name.empty()) {
        ereport(pgcpp::error::LogLevel::kError, "CREATE DOMAIN: missing base type");
    }

    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "CREATE DOMAIN: catalog not initialized");
    }

    // Look up the base type.
    const FormData_pg_type* base_type = cat->GetTypeByName(base_type_name);
    if (base_type == nullptr) {
        ereport(pgcpp::error::LogLevel::kError,
                "CREATE DOMAIN: type \"" + base_type_name + "\" does not exist");
    }

    // Check for duplicate domain name.
    if (cat->GetTypeByName(domain_name) != nullptr) {
        ereport(pgcpp::error::LogLevel::kError,
                "CREATE DOMAIN: type \"" + domain_name + "\" already exists");
    }

    // Build the pg_type row for a domain type.
    auto* type_row = makePallocNode<FormData_pg_type>();
    type_row->oid = pgcpp::catalog::kInvalidOid;  // assigned by InsertType
    type_row->typname = domain_name;
    type_row->typlen = base_type->typlen;
    type_row->typbyval = base_type->typbyval;
    type_row->typtype = TypeType::kDomain;
    type_row->typcategory = base_type->typcategory;
    type_row->typispreferred = false;
    type_row->typisdefined = true;
    type_row->typalign = base_type->typalign;
    type_row->typstorage = base_type->typstorage;
    type_row->typbasetype = base_type->oid;
    type_row->typtypmod = -1;
    type_row->typcollation = base_type->typcollation;
    type_row->typnotnull = stmt->not_null;

    cat->InsertType(type_row);

    return "CREATE DOMAIN";
}

}  // namespace pgcpp::commands

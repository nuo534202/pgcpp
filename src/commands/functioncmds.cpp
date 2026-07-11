// functioncmds.cpp — CREATE FUNCTION implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/functioncmds.c.
//
// Parses a CreateFunctionStmt, extracts options (LANGUAGE, AS, volatility,
// strict), builds a FormData_pg_proc row, and persists it via the Catalog.
// This enables fmgr_info() to look up user-created functions by OID.
#include "commands/functioncmds.hpp"

#include <string>
#include <vector>

#include "catalog/catalog.hpp"
#include "catalog/pg_proc.hpp"
#include "catalog/pg_type.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "parser/parsenodes.hpp"
#include "utils/fmgr.hpp"

namespace pgcpp::commands {

using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_proc;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::Oid;
using pgcpp::catalog::ProKind;
using pgcpp::catalog::ProVolatile;
using pgcpp::fmgr::kCLanguageOid;
using pgcpp::fmgr::kInternalLanguageOid;
using pgcpp::fmgr::kSqlLanguageOid;
using pgcpp::nodes::makePallocNode;
using pgcpp::nodes::Node;
using pgcpp::nodes::NodeTag;
using pgcpp::nodes::Value;
using pgcpp::parser::CreateFunctionStmt;
using pgcpp::parser::DefElem;
using pgcpp::parser::TypeName;

namespace {

// Extract the last component of a qualified name (e.g., "myfunc" from
// "public.myfunc"). The names list contains String Value nodes.
std::string ExtractLastName(const std::vector<Node*>& names) {
    if (names.empty())
        return "";
    const Node* last = names.back();
    if (last == nullptr || last->GetTag() != NodeTag::kString)
        return "";
    return static_cast<const Value*>(last)->GetString();
}

// Resolve a language name to its OID.
Oid ResolveLanguageOid(const std::string& langname) {
    if (langname == "internal")
        return kInternalLanguageOid;
    if (langname == "c")
        return kCLanguageOid;
    if (langname == "sql")
        return kSqlLanguageOid;
    return pgcpp::catalog::kInvalidOid;
}

// Resolve a type name to its OID via the catalog.
Oid ResolveTypeOid(const std::string& type_name) {
    Catalog* cat = GetCatalog();
    if (cat == nullptr)
        return pgcpp::catalog::kInvalidOid;
    const auto* type = cat->GetTypeByName(type_name);
    if (type != nullptr)
        return type->oid;
    return pgcpp::catalog::kInvalidOid;
}

// Extract argument type OIDs from the parameter list (TypeName nodes).
std::vector<Oid> ExtractArgTypes(const std::vector<Node*>& parameters) {
    std::vector<Oid> argtypes;
    argtypes.reserve(parameters.size());
    for (const Node* param : parameters) {
        if (param == nullptr)
            continue;
        if (param->GetTag() == NodeTag::kTypeName) {
            const auto* tn = static_cast<const TypeName*>(param);
            std::string tname = ExtractLastName(tn->names);
            if (!tname.empty())
                argtypes.push_back(ResolveTypeOid(tname));
        }
    }
    return argtypes;
}

// Parse the options list and fill the corresponding pg_proc fields.
void ParseOptions(const std::vector<Node*>& options, FormData_pg_proc* row) {
    for (const Node* opt : options) {
        if (opt == nullptr || opt->GetTag() != NodeTag::kDefElem)
            continue;
        const auto* de = static_cast<const DefElem*>(opt);

        if (de->defname == "language") {
            if (de->arg != nullptr &&
                de->arg->GetTag() == NodeTag::kString) {
                std::string lang =
                    static_cast<const Value*>(de->arg)->GetString();
                row->prolang = ResolveLanguageOid(lang);
            }
        } else if (de->defname == "as") {
            // AS Sconst — the function body source text (SQL or C symbol).
            if (de->arg != nullptr &&
                de->arg->GetTag() == NodeTag::kString) {
                row->prosrc =
                    static_cast<const Value*>(de->arg)->GetString();
            }
        } else if (de->defname == "volatility") {
            if (de->arg != nullptr &&
                de->arg->GetTag() == NodeTag::kString) {
                std::string vol =
                    static_cast<const Value*>(de->arg)->GetString();
                if (vol == "immutable")
                    row->provolatile = ProVolatile::kImmutable;
                else if (vol == "stable")
                    row->provolatile = ProVolatile::kStable;
                else
                    row->provolatile = ProVolatile::kVolatile;
            }
        } else if (de->defname == "strict") {
            // STRICT flag: arg is an Integer Value (1 = strict).
            if (de->arg != nullptr &&
                de->arg->GetTag() == NodeTag::kInteger) {
                row->proisstrict =
                    static_cast<const Value*>(de->arg)->GetInteger() != 0;
            }
        }
        // Other options (parallel, cost, rows, security_definer, leakproof,
        // window, support) are parsed but not yet acted upon.
    }
}

}  // namespace

std::string CreateFunction(CreateFunctionStmt* stmt) {
    if (stmt == nullptr)
        return "CREATE FUNCTION";

    // Extract function name.
    std::string proname = ExtractLastName(stmt->funcname);
    if (proname.empty()) {
        ereport(pgcpp::error::LogLevel::kError,
                "CREATE FUNCTION: no function name given");
        return "CREATE FUNCTION";
    }

    // Build the pg_proc row.
    auto* row = makePallocNode<FormData_pg_proc>();
    row->proname = proname;
    row->prokind = stmt->is_procedure ? ProKind::kProcedure : ProKind::kFunction;

    // Resolve return type.
    if (stmt->return_type != nullptr) {
        std::string retname = ExtractLastName(stmt->return_type->names);
        row->prorettype = ResolveTypeOid(retname);
        row->proretset = stmt->return_type->setof;
    }

    // Extract argument types.
    row->proargtypes = ExtractArgTypes(stmt->parameters);
    row->pronargs = static_cast<int16_t>(row->proargtypes.size());

    // Defaults: prolang=kInvalidOid (internal), proisstrict=true,
    // provolatile=immutable — matching PostgreSQL's pg_proc defaults.
    // ParseOptions overrides these based on the statement's options.
    row->prolang = kInternalLanguageOid;
    row->proisstrict = true;
    row->provolatile = ProVolatile::kVolatile;

    // Parse options (LANGUAGE, AS, volatility, strict, ...).
    ParseOptions(stmt->options, row);

    // Persist via catalog.
    Catalog* cat = GetCatalog();
    if (cat != nullptr) {
        cat->InsertProc(row);
    }

    return stmt->is_procedure ? "CREATE PROCEDURE" : "CREATE FUNCTION";
}

}  // namespace pgcpp::commands

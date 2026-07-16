// extensioncmds.cpp — CREATE/DROP EXTENSION implementation (P3-10).
//
// Converted from PostgreSQL 15's src/backend/commands/extension.c.
//
// CREATE EXTENSION:
//   1. Look up the control file in the extension registry.
//   2. Resolve the version (default_version if not specified).
//   3. If CASCADE, recursively install required extensions.
//   4. Execute the extension's SQL script (if non-empty).
//   5. Insert a pg_extension row.
//
// DROP EXTENSION:
//   1. For each named extension, look up the pg_extension row.
//   2. If IF EXISTS and not found, skip silently.
//   3. Remove the pg_extension row.
//
// Simplifications from PostgreSQL:
//   - No shared library loading (extensions are pure SQL or built-in).
//   - SQL script execution is not wired to the parser here; the script
//     content is stored but execution would require a live server context.
//     Tests verify the catalog and registry mechanics instead.
//   - CASCADE for DROP only removes the pg_extension row, not dependent
//     objects (full dependency tracking is a future task).
#include "commands/extensioncmds.hpp"

#include <string>
#include <vector>

#include "catalog/catalog.hpp"
#include "catalog/pg_extension.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "extension/extension.hpp"
#include "parser/parsenodes.hpp"

namespace pgcpp::commands {

using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_extension;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::Oid;
using pgcpp::extension::ExtensionControlFile;
using pgcpp::extension::ExtensionRegistry;
using pgcpp::extension::GetExtensionRegistry;
using pgcpp::nodes::makePallocNode;
using pgcpp::parser::CreateExtensionStmt;
using pgcpp::parser::DropExtensionStmt;

namespace {

// Recursively install an extension and its dependencies. Returns the OID
// assigned to the pg_extension row, or kInvalidOid on error.
Oid InstallExtension(const std::string& extname, const std::string& requested_version,
                     const std::string& schema, bool cascade) {
    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "CREATE EXTENSION: catalog not initialized");
        return pgcpp::catalog::kInvalidOid;
    }

    ExtensionRegistry& reg = GetExtensionRegistry();
    const ExtensionControlFile* ctrl = reg.GetControlFile(extname);
    if (ctrl == nullptr) {
        ereport(pgcpp::error::LogLevel::kError,
                "CREATE EXTENSION: extension \"" + extname + "\" is not available");
        return pgcpp::catalog::kInvalidOid;
    }

    // Resolve version.
    std::string version = requested_version.empty() ? ctrl->default_version : requested_version;
    if (version.empty()) {
        ereport(pgcpp::error::LogLevel::kError,
                "CREATE EXTENSION: extension \"" + extname + "\" has no default version");
        return pgcpp::catalog::kInvalidOid;
    }

    // If CASCADE, install required extensions first.
    if (cascade) {
        for (const auto& req : ctrl->requires_list) {
            if (cat->GetExtensionByName(req) == nullptr) {
                // Recursively install the dependency.
                Oid dep_oid = InstallExtension(req, "", schema, true);
                if (dep_oid == pgcpp::catalog::kInvalidOid) {
                    // Error already reported by the recursive call.
                    return pgcpp::catalog::kInvalidOid;
                }
            }
        }
    }

    // Insert the pg_extension row.
    auto* row = makePallocNode<FormData_pg_extension>();
    row->extname = extname;
    row->extversion = version;
    row->extrelocatable = ctrl->relocatable;
    // extowner and extnamespace are not enforced in this simplified impl.
    // The target schema (if specified) would resolve to extnamespace via
    // pg_namespace lookup in a full implementation.
    (void)schema;
    // extconfig and extcondition are empty (no configuration tables).

    return cat->InsertExtension(row);
}

}  // namespace

std::string CreateExtension(CreateExtensionStmt* stmt) {
    if (stmt == nullptr)
        return "CREATE EXTENSION";

    if (stmt->extname.empty()) {
        ereport(pgcpp::error::LogLevel::kError, "CREATE EXTENSION: no extension name given");
        return "CREATE EXTENSION";
    }

    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "CREATE EXTENSION: catalog not initialized");
        return "CREATE EXTENSION";
    }

    // Check if already installed.
    const FormData_pg_extension* existing = cat->GetExtensionByName(stmt->extname);
    if (existing != nullptr) {
        if (stmt->if_not_exists) {
            // Silently skip — note that PostgreSQL emits a NOTICE here.
            return "CREATE EXTENSION";
        }
        ereport(pgcpp::error::LogLevel::kError,
                "CREATE EXTENSION: extension \"" + stmt->extname + "\" already exists");
        return "CREATE EXTENSION";
    }

    Oid oid = InstallExtension(stmt->extname, stmt->version, stmt->schema, stmt->cascade);
    if (oid == pgcpp::catalog::kInvalidOid) {
        // Error already reported by InstallExtension.
        return "CREATE EXTENSION";
    }

    return "CREATE EXTENSION";
}

std::string DropExtension(DropExtensionStmt* stmt) {
    if (stmt == nullptr)
        return "DROP EXTENSION";

    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "DROP EXTENSION: catalog not initialized");
        return "DROP EXTENSION";
    }

    for (const auto& extname : stmt->extnames) {
        const FormData_pg_extension* row = cat->GetExtensionByName(extname);
        if (row == nullptr) {
            if (stmt->missing_ok)
                continue;
            ereport(pgcpp::error::LogLevel::kError,
                    "DROP EXTENSION: extension \"" + extname + "\" does not exist");
            return "DROP EXTENSION";
        }
        cat->DeleteExtension(row->oid);
        // Note: CASCADE for dependent objects is not implemented in this
        // simplified version. Only the pg_extension row is removed.
    }

    return "DROP EXTENSION";
}

}  // namespace pgcpp::commands

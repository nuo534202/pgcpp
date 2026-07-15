// languagecmds.cpp — CREATE/DROP LANGUAGE and DO implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/proclang.c.
//
// Implements the user-facing commands for the PL language framework:
//   * CREATE LANGUAGE inserts a pg_language row. The handler/inline/validator
//     function names are resolved to OIDs (best-effort: if no catalog match,
//     kInvalidOid is recorded, mirroring PostgreSQL's behavior when the
//     function is missing).
//   * DROP LANGUAGE removes the row, honoring IF EXISTS.
//   * DO looks up the language's inline handler via the PL handler registry
//     and invokes it with the code string.
#include "commands/languagecmds.hpp"

#include <string>
#include <vector>

#include "catalog/catalog.hpp"
#include "catalog/pg_language.hpp"
#include "catalog/pg_proc.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/memory_context.hpp"
#include "parser/parsenodes.hpp"
#include "pl/pl_handler.hpp"
#include "utils/fmgr.hpp"

namespace pgcpp::commands {

using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_language;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::Oid;
using pgcpp::fmgr::kPlPgsqlLanguageOid;
using pgcpp::nodes::makePallocNode;
using pgcpp::nodes::Node;
using pgcpp::nodes::NodeTag;
using pgcpp::nodes::Value;
using pgcpp::parser::CreateLanguageStmt;
using pgcpp::parser::DoStmt;
using pgcpp::parser::DropLanguageStmt;
using pgcpp::pl::LookupPlHandlerByName;

namespace {

// Extract the last component of a qualified name list (e.g., "myhandler"
// from "public.myhandler"). The names list contains String Value nodes.
std::string ExtractLastName(const std::vector<Node*>& names) {
    if (names.empty())
        return "";
    const Node* last = names.back();
    if (last == nullptr || last->GetTag() != NodeTag::kString)
        return "";
    return static_cast<const Value*>(last)->GetString();
}

// Resolve a function name to its OID by looking it up in pg_proc.
Oid ResolveFunctionOid(const std::vector<Node*>& names) {
    if (names.empty())
        return pgcpp::catalog::kInvalidOid;
    std::string fname = ExtractLastName(names);
    if (fname.empty())
        return pgcpp::catalog::kInvalidOid;
    Catalog* cat = GetCatalog();
    if (cat == nullptr)
        return pgcpp::catalog::kInvalidOid;
    auto procs = cat->GetProcsByName(fname);
    if (procs.empty())
        return pgcpp::catalog::kInvalidOid;
    return procs[0]->oid;
}

}  // namespace

std::string CreateLanguage(CreateLanguageStmt* stmt) {
    if (stmt == nullptr)
        return "CREATE LANGUAGE";

    if (stmt->lanname.empty()) {
        ereport(pgcpp::error::LogLevel::kError, "CREATE LANGUAGE: no language name given");
        return "CREATE LANGUAGE";
    }

    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "CREATE LANGUAGE: catalog not initialized");
        return "CREATE LANGUAGE";
    }

    // Reject duplicates unless REPLACE was specified.
    const auto* existing = cat->GetLanguageByName(stmt->lanname);
    if (existing != nullptr && !stmt->replace) {
        ereport(pgcpp::error::LogLevel::kError,
                "CREATE LANGUAGE: language \"" + stmt->lanname + "\" already exists");
        return "CREATE LANGUAGE";
    }
    if (existing != nullptr && stmt->replace) {
        // DROP the existing row so we can insert a fresh one.
        cat->DeleteLanguage(existing->oid);
    }

    auto* row = makePallocNode<FormData_pg_language>();
    row->lanname = stmt->lanname;
    row->lanpltrusted = stmt->trusted;
    // Built-in plpgsql pins OID 100; new languages get a fresh OID from the
    // catalog. We also mark them as PL languages (lanispl=true).
    if (stmt->lanname == "plpgsql") {
        row->oid = kPlPgsqlLanguageOid;
    }
    row->lanispl = true;
    row->lanplcallfoid = ResolveFunctionOid(stmt->plhandler);
    row->laninlinefoid = ResolveFunctionOid(stmt->inline_handler);
    row->lanvalidator = ResolveFunctionOid(stmt->validator);

    cat->InsertLanguage(row);
    return "CREATE LANGUAGE";
}

std::string DropLanguage(DropLanguageStmt* stmt) {
    if (stmt == nullptr)
        return "DROP LANGUAGE";

    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "DROP LANGUAGE: catalog not initialized");
        return "DROP LANGUAGE";
    }

    const auto* row = cat->GetLanguageByName(stmt->lanname);
    if (row == nullptr) {
        if (stmt->missing_ok)
            return "DROP LANGUAGE";
        ereport(pgcpp::error::LogLevel::kError,
                "DROP LANGUAGE: language \"" + stmt->lanname + "\" does not exist");
        return "DROP LANGUAGE";
    }
    cat->DeleteLanguage(row->oid);
    return "DROP LANGUAGE";
}

std::string DoBlock(DoStmt* stmt) {
    if (stmt == nullptr)
        return "DO";

    std::string lanname = stmt->lanname.empty() ? "plpgsql" : stmt->lanname;
    const auto* handler = LookupPlHandlerByName(lanname);
    if (handler == nullptr) {
        ereport(pgcpp::error::LogLevel::kError,
                "DO: language \"" + lanname + "\" does not have a registered handler");
        return "DO";
    }
    if (handler->inline_cb == nullptr) {
        ereport(pgcpp::error::LogLevel::kError,
                "DO: language \"" + lanname + "\" does not support inline code blocks");
        return "DO";
    }
    handler->inline_cb(stmt->code);
    return "DO";
}

}  // namespace pgcpp::commands

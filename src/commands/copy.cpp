// copy.cpp — COPY command router.
//
// Converted from PostgreSQL 15's src/backend/commands/copy.c.
// Dispatches COPY FROM (CopyFromText) and COPY TO (CopyToText).
#include "pgcpp/commands/copy.hpp"

#include <string>

#include "pgcpp/access/rel.hpp"
#include "pgcpp/catalog/catalog.hpp"
#include "pgcpp/catalog/pg_class.hpp"
#include "pgcpp/commands/copy_from.hpp"
#include "pgcpp/commands/copy_to.hpp"
#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/parser/parsenodes.hpp"

namespace pgcpp::commands {

using pgcpp::access::Relation;
using pgcpp::access::RelationClose;
using pgcpp::access::RelationOpen;
using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_class;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::Oid;
using pgcpp::parser::CopyStmt;

std::string DoCopy(CopyStmt* stmt) {
    if (stmt == nullptr || stmt->relation == nullptr)
        return "COPY 0";

    Catalog* cat = GetCatalog();
    if (cat == nullptr)
        return "COPY 0";

    const std::string& relname = stmt->relation->relname;
    const FormData_pg_class* class_row = cat->GetClassByName(relname);
    if (class_row == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "relation \"" + relname + "\" does not exist");
    }
    Oid relid = class_row->oid;
    Relation rel = RelationOpen(relid);
    if (rel == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "cannot open relation \"" + relname + "\"");
    }

    int64_t row_count = 0;
    if (stmt->is_from) {
        row_count = CopyFromText(rel, stmt->filename);
    } else {
        row_count = CopyToText(rel, stmt->filename);
    }

    RelationClose(rel);
    return "COPY " + std::to_string(row_count);
}

}  // namespace pgcpp::commands

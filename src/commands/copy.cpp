// copy.cpp — COPY command router.
//
// Converted from PostgreSQL 15's src/backend/commands/copy.c.
// Dispatches COPY FROM (CopyFromText) and COPY TO (CopyToText).
#include "mytoydb/commands/copy.hpp"

#include <string>

#include "mytoydb/access/rel.hpp"
#include "mytoydb/catalog/catalog.hpp"
#include "mytoydb/catalog/pg_class.hpp"
#include "mytoydb/commands/copy_from.hpp"
#include "mytoydb/commands/copy_to.hpp"
#include "mytoydb/common/error/elog.hpp"
#include "mytoydb/parser/parsenodes.hpp"

namespace mytoydb::commands {

using mytoydb::access::Relation;
using mytoydb::access::RelationClose;
using mytoydb::access::RelationOpen;
using mytoydb::catalog::Catalog;
using mytoydb::catalog::FormData_pg_class;
using mytoydb::catalog::GetCatalog;
using mytoydb::catalog::Oid;
using mytoydb::parser::CopyStmt;

std::string DoCopy(CopyStmt* stmt) {
    if (stmt == nullptr || stmt->relation == nullptr)
        return "COPY 0";

    Catalog* cat = GetCatalog();
    if (cat == nullptr)
        return "COPY 0";

    const std::string& relname = stmt->relation->relname;
    const FormData_pg_class* class_row = cat->GetClassByName(relname);
    if (class_row == nullptr) {
        ereport(mytoydb::error::LogLevel::kError, "relation \"" + relname + "\" does not exist");
    }
    Oid relid = class_row->oid;
    Relation rel = RelationOpen(relid);
    if (rel == nullptr) {
        ereport(mytoydb::error::LogLevel::kError, "cannot open relation \"" + relname + "\"");
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

}  // namespace mytoydb::commands

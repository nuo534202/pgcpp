// sequence.cpp — CREATE SEQUENCE implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/sequence.c.
// Creates a relation of relkind 'S' (sequence). MyToyDB doesn't yet
// implement nextval()/currval() — this stub records the catalog entry
// so CREATE SEQUENCE doesn't fail.
#include "mytoydb/commands/sequence.hpp"

#include <string>

#include "mytoydb/access/rel.hpp"
#include "mytoydb/catalog/catalog.hpp"
#include "mytoydb/catalog/pg_class.hpp"
#include "mytoydb/common/containers/node.hpp"
#include "mytoydb/common/error/elog.hpp"
#include "mytoydb/common/memory/memory_context.hpp"
#include "mytoydb/parser/parsenodes.hpp"

namespace mytoydb::commands {

using mytoydb::access::RelationCreateStorage;
using mytoydb::catalog::Catalog;
using mytoydb::catalog::FormData_pg_class;
using mytoydb::catalog::GetCatalog;
using mytoydb::catalog::Oid;
using mytoydb::catalog::RelKind;
using mytoydb::catalog::RelPersistence;
using mytoydb::nodes::makePallocNode;
using mytoydb::parser::CreateSeqStmt;

std::string DefineSequence(CreateSeqStmt* stmt) {
    if (stmt == nullptr || stmt->sequence == nullptr)
        return "";

    Catalog* cat = GetCatalog();
    if (cat == nullptr)
        return "";

    const std::string& seqname = stmt->sequence->relname;

    if (cat->GetClassByName(seqname) != nullptr) {
        if (stmt->if_not_exists)
            return "CREATE SEQUENCE";
        ereport(mytoydb::error::LogLevel::kError, "relation \"" + seqname + "\" already exists");
    }

    auto* class_row = makePallocNode<FormData_pg_class>();
    class_row->relname = seqname;
    class_row->relnamespace = 2200;
    class_row->relkind = RelKind::kSequence;
    class_row->relpersistence = RelPersistence::kPermanent;
    class_row->relnatts = 1;  // sequences have a single "last_value" column
    class_row->relispopulated = true;
    Oid relid = cat->InsertClass(class_row);
    class_row->relfilenode = relid;

    RelationCreateStorage(relid, false);
    return "CREATE SEQUENCE";
}

}  // namespace mytoydb::commands

// sequence.cpp — CREATE SEQUENCE implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/sequence.c.
// Creates a relation of relkind 'S' (sequence). pgcpp doesn't yet
// implement nextval()/currval() — this stub records the catalog entry
// so CREATE SEQUENCE doesn't fail.
#include "commands/sequence.hpp"

#include <string>

#include "access/rel.hpp"
#include "catalog/catalog.hpp"
#include "catalog/pg_class.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/memory_context.hpp"
#include "parser/parsenodes.hpp"

namespace pgcpp::commands {

using pgcpp::access::RelationCreateStorage;
using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_class;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::Oid;
using pgcpp::catalog::RelKind;
using pgcpp::catalog::RelPersistence;
using pgcpp::nodes::makePallocNode;
using pgcpp::parser::CreateSeqStmt;

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
        ereport(pgcpp::error::LogLevel::kError, "relation \"" + seqname + "\" already exists");
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

}  // namespace pgcpp::commands

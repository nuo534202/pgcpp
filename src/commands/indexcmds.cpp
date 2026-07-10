// indexcmds.cpp — CREATE INDEX implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/indexcmds.c.
// Extracted from src/protocol/utility.cpp to the commands/ module.
#include "commands/indexcmds.hpp"

#include <cstdint>
#include <string>

#include "access/indexam.hpp"
#include "access/nbtree.hpp"
#include "access/rel.hpp"
#include "catalog/catalog.hpp"
#include "catalog/lsyscache.hpp"
#include "catalog/pg_am.hpp"
#include "catalog/pg_attribute.hpp"
#include "catalog/pg_class.hpp"
#include "catalog/pg_index.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/memory_context.hpp"
#include "parser/parse_type.hpp"
#include "parser/parsenodes.hpp"
#include "types/builtins.hpp"

namespace pgcpp::commands {

using pgcpp::access::BTKeyKind;
using pgcpp::access::index_build;
using pgcpp::access::kBrinAmOid;
using pgcpp::access::kBTreeAmOid;
using pgcpp::access::kGinAmOid;
using pgcpp::access::kGistAmOid;
using pgcpp::access::kHashAmOid;
using pgcpp::access::kSpgistAmOid;
using pgcpp::access::Relation;
using pgcpp::access::RelationClose;
using pgcpp::access::RelationCreateStorage;
using pgcpp::access::RelationOpen;
using pgcpp::catalog::AttAlign;
using pgcpp::catalog::AttStorage;
using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_am;
using pgcpp::catalog::FormData_pg_attribute;
using pgcpp::catalog::FormData_pg_class;
using pgcpp::catalog::FormData_pg_index;
using pgcpp::catalog::get_typalign;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::Oid;
using pgcpp::catalog::RelKind;
using pgcpp::catalog::RelPersistence;
using pgcpp::nodes::makePallocNode;
using pgcpp::nodes::NodeTag;
using pgcpp::parser::IndexElem;
using pgcpp::parser::IndexStmt;
using pgcpp::parser::Node;
using pgcpp::types::kInt4Oid;

namespace {

BTKeyKind BtKeyKindForType(Oid type_oid) {
    switch (type_oid) {
        case pgcpp::types::kInt2Oid:
        case pgcpp::types::kInt4Oid:
            return BTKeyKind::kInt32;
        case pgcpp::types::kInt8Oid:
            return BTKeyKind::kInt64;
        case pgcpp::types::kTextOid:
        case pgcpp::types::kVarcharOid:
            return BTKeyKind::kText;
        default:
            return BTKeyKind::kInt32;
    }
}

}  // namespace

std::string DefineIndex(IndexStmt* stmt) {
    if (stmt == nullptr || stmt->relation == nullptr)
        return "";

    Catalog* cat = GetCatalog();
    if (cat == nullptr)
        return "";

    const std::string& heapname = stmt->relation->relname;
    const FormData_pg_class* heap_row = cat->GetClassByName(heapname);
    if (heap_row == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "relation \"" + heapname + "\" does not exist");
    }
    Oid heap_oid = heap_row->oid;

    Oid key_type_oid = kInt4Oid;
    if (!stmt->index_params.empty()) {
        Node* first = stmt->index_params[0];
        if (first != nullptr && first->GetTag() == NodeTag::kIndexElem) {
            auto* elem = static_cast<IndexElem*>(first);
            if (!elem->name.empty()) {
                auto attrs = cat->GetAttributes(heap_oid);
                for (const FormData_pg_attribute* attr : attrs) {
                    if (attr->attname == elem->name) {
                        key_type_oid = attr->atttypid;
                        break;
                    }
                }
            }
        }
    }
    BTKeyKind key_kind = BtKeyKindForType(key_type_oid);

    std::string idxname = stmt->idxname;
    if (idxname.empty())
        idxname = heapname + "_idx";

    if (cat->GetClassByName(idxname) != nullptr) {
        if (stmt->if_not_exists)
            return "CREATE INDEX";
        ereport(pgcpp::error::LogLevel::kError, "relation \"" + idxname + "\" already exists");
    }

    // Resolve the access method: use the one named in stmt->access_method,
    // or default to btree when unspecified (e.g. CREATE INDEX without USING).
    std::string am_name = stmt->access_method;
    if (am_name.empty())
        am_name = "btree";
    const FormData_pg_am* am_row = cat->GetAmByName(am_name);
    Oid am_oid;
    if (am_row != nullptr) {
        am_oid = am_row->oid;
    } else {
        // Fallback for catalogs that were not bootstrapped with pg_am rows:
        // resolve well-known AM names to their canonical OIDs.
        if (am_name == "btree")
            am_oid = kBTreeAmOid;
        else if (am_name == "hash")
            am_oid = kHashAmOid;
        else if (am_name == "gist")
            am_oid = kGistAmOid;
        else if (am_name == "gin")
            am_oid = kGinAmOid;
        else if (am_name == "brin")
            am_oid = kBrinAmOid;
        else if (am_name == "spgist")
            am_oid = kSpgistAmOid;
        else
            ereport(pgcpp::error::LogLevel::kError,
                    "access method \"" + am_name + "\" does not exist");
    }

    auto* class_row = makePallocNode<FormData_pg_class>();
    class_row->relname = idxname;
    class_row->relnamespace = 2200;
    class_row->relkind = RelKind::kIndex;
    class_row->relpersistence = RelPersistence::kPermanent;
    class_row->relnatts = static_cast<int16_t>(stmt->index_params.size());
    class_row->relispopulated = true;
    class_row->relam = am_oid;  // record the AM on the index relation
    Oid index_oid = cat->InsertClass(class_row);
    class_row->relfilenode = index_oid;

    int16_t attnum = 1;
    for (Node* node : stmt->index_params) {
        if (node == nullptr || node->GetTag() != NodeTag::kIndexElem)
            continue;
        auto* elem = static_cast<IndexElem*>(node);
        Oid col_type = kInt4Oid;
        int16_t col_len = 4;
        bool col_byval = true;
        auto attrs = cat->GetAttributes(heap_oid);
        for (const FormData_pg_attribute* attr : attrs) {
            if (attr->attname == elem->name) {
                col_type = attr->atttypid;
                col_len = attr->attlen;
                col_byval = attr->attbyval;
                break;
            }
        }
        auto* attr_row = makePallocNode<FormData_pg_attribute>();
        attr_row->attrelid = index_oid;
        attr_row->attname = elem->name;
        attr_row->atttypid = col_type;
        attr_row->attlen = col_len;
        attr_row->attnum = attnum;
        attr_row->attbyval = col_byval;
        attr_row->attstorage = AttStorage::kPlain;
        attr_row->attalign = static_cast<AttAlign>(get_typalign(col_type));
        attr_row->attnotnull = false;
        attr_row->attislocal = true;
        cat->InsertAttribute(attr_row);
        attnum++;
    }

    RelationCreateStorage(index_oid, false);
    Relation index_rel = RelationOpen(index_oid);
    if (index_rel != nullptr) {
        index_build(index_rel, key_kind);
        RelationClose(index_rel);
    }

    // Record the index in pg_index so the executor/constraint code can
    // discover it (uniqueness, primary key, key columns).
    auto* idx_row = makePallocNode<FormData_pg_index>();
    idx_row->indexrelid = index_oid;
    idx_row->indrelid = heap_oid;
    idx_row->indnatts = static_cast<int16_t>(stmt->index_params.size());
    idx_row->indnkeyatts = idx_row->indnatts;
    idx_row->indisunique = stmt->unique;
    idx_row->indisprimary = stmt->primary;
    idx_row->indisimmediate = true;
    idx_row->indisvalid = true;
    idx_row->indisready = true;
    idx_row->indislive = true;
    int16_t key_attnum = 1;
    for (Node* node : stmt->index_params) {
        if (node == nullptr || node->GetTag() != NodeTag::kIndexElem)
            continue;
        auto* elem = static_cast<IndexElem*>(node);
        auto attrs = cat->GetAttributes(heap_oid);
        for (const FormData_pg_attribute* attr : attrs) {
            if (attr->attname == elem->name) {
                idx_row->indkey.push_back(attr->attnum);
                break;
            }
        }
        (void)key_attnum;
        key_attnum++;
    }
    cat->InsertIndex(idx_row);

    return "CREATE INDEX";
}

}  // namespace pgcpp::commands

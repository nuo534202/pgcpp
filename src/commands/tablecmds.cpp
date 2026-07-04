// tablecmds.cpp — CREATE/ALTER/DROP TABLE / TRUNCATE implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/tablecmds.c.
// These handlers were previously inline in src/protocol/utility.cpp;
// they have been moved here so utility.cpp can be a thin dispatcher
// (matching PostgreSQL's tcop/utility.c + commands/* separation).
#include "commands/tablecmds.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "access/heapam.hpp"
#include "access/rel.hpp"
#include "catalog/catalog.hpp"
#include "catalog/lsyscache.hpp"
#include "catalog/pg_attribute.hpp"
#include "catalog/pg_class.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/memory_context.hpp"
#include "parser/parse_type.hpp"
#include "parser/parsenodes.hpp"
#include "storage/bufmgr.hpp"
#include "storage/smgr.hpp"
#include "types/builtins.hpp"

namespace pgcpp::commands {

using pgcpp::access::Relation;
using pgcpp::access::RelationClose;
using pgcpp::access::RelationCreateStorage;
using pgcpp::access::RelationDropStorage;
using pgcpp::access::RelationOpen;
using pgcpp::catalog::AttAlign;
using pgcpp::catalog::AttStorage;
using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_attribute;
using pgcpp::catalog::FormData_pg_class;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::get_typalign;
using pgcpp::catalog::Oid;
using pgcpp::catalog::RelKind;
using pgcpp::catalog::RelPersistence;
using pgcpp::memory::palloc;
using pgcpp::memory::pfree;
using pgcpp::nodes::makePallocNode;
using pgcpp::nodes::NodeTag;
using pgcpp::parser::AlterTableCmd;
using pgcpp::parser::AlterTableStmt;
using pgcpp::parser::ColumnDef;
using pgcpp::parser::CreateStmt;
using pgcpp::parser::DropStmt;
using pgcpp::parser::get_typbyval;
using pgcpp::parser::get_typlen;
using pgcpp::parser::Node;
using pgcpp::parser::RangeVar;
using pgcpp::parser::RenameStmt;
using pgcpp::parser::TruncateStmt;
using pgcpp::parser::TypeName;
using pgcpp::parser::typenameTypeId;
using pgcpp::storage::smgrdounlinkall;

namespace {

// Extract the type name string from a TypeName node.
std::string ExtractTypeName(TypeName* type_name) {
    if (type_name == nullptr || type_name->names.empty())
        return "";
    Node* last = type_name->names.back();
    if (last->GetTag() == NodeTag::kString) {
        auto* v = static_cast<pgcpp::nodes::Value*>(last);
        return v->GetString();
    }
    return "";
}

}  // namespace

std::string DefineRelation(CreateStmt* stmt) {
    if (stmt == nullptr || stmt->relation == nullptr)
        return "";

    Catalog* cat = GetCatalog();
    if (cat == nullptr)
        return "";

    const std::string& relname = stmt->relation->relname;

    if (cat->GetClassByName(relname) != nullptr) {
        if (stmt->if_not_exists)
            return "CREATE TABLE";
        ereport(pgcpp::error::LogLevel::kError, "relation \"" + relname + "\" already exists");
    }

    // Resolve column types.
    int16_t natts = 0;
    struct ColInfo {
        std::string name;
        Oid type_oid;
        int16_t typlen;
        bool typbyval;
        bool is_not_null;
    };
    std::vector<ColInfo> columns;

    for (Node* elt : stmt->table_elts) {
        if (elt == nullptr || elt->GetTag() != NodeTag::kColumnDef)
            continue;
        auto* coldef = static_cast<ColumnDef*>(elt);

        ColInfo ci;
        ci.name = coldef->colname;
        ci.is_not_null = coldef->is_not_null;
        std::string type_name = ExtractTypeName(coldef->type_name);
        ci.type_oid = typenameTypeId(type_name);
        if (ci.type_oid == pgcpp::types::kInvalidOid) {
            ereport(pgcpp::error::LogLevel::kError, "type \"" + type_name + "\" does not exist");
        }
        ci.typlen = get_typlen(ci.type_oid);
        ci.typbyval = get_typbyval(ci.type_oid);
        columns.push_back(ci);
        natts++;
    }

    // Create the pg_class entry.
    auto* class_row = makePallocNode<FormData_pg_class>();
    class_row->relname = relname;
    class_row->relnamespace = 2200;  // public schema
    class_row->relkind = RelKind::kRelation;
    class_row->relpersistence = RelPersistence::kPermanent;
    class_row->relnatts = natts;
    class_row->relispopulated = true;

    Oid relid = cat->InsertClass(class_row);
    class_row->relfilenode = relid;

    // Create pg_attribute entries.
    int16_t attnum = 1;
    for (const auto& ci : columns) {
        auto* attr_row = makePallocNode<FormData_pg_attribute>();
        attr_row->attrelid = relid;
        attr_row->attname = ci.name;
        attr_row->atttypid = ci.type_oid;
        attr_row->attlen = ci.typlen;
        attr_row->attnum = attnum;
        attr_row->attbyval = ci.typbyval;
        attr_row->attstorage = AttStorage::kPlain;
        attr_row->attalign = static_cast<AttAlign>(get_typalign(ci.type_oid));
        attr_row->attnotnull = ci.is_not_null;
        attr_row->attislocal = true;
        cat->InsertAttribute(attr_row);
        attnum++;
    }

    RelationCreateStorage(relid, false);
    return "CREATE TABLE";
}

std::string RemoveRelations(DropStmt* stmt) {
    if (stmt == nullptr)
        return "";

    std::string tag;
    switch (stmt->remove_type) {
        case pgcpp::parser::ObjectType::kTable:
            tag = "DROP TABLE";
            break;
        case pgcpp::parser::ObjectType::kIndex:
            tag = "DROP INDEX";
            break;
        case pgcpp::parser::ObjectType::kView:
            tag = "DROP VIEW";
            break;
        default:
            tag = "DROP";
            break;
    }

    Catalog* cat = GetCatalog();
    if (cat == nullptr)
        return tag;

    for (Node* obj : stmt->objects) {
        if (obj == nullptr || obj->GetTag() != NodeTag::kString)
            continue;
        auto* v = static_cast<pgcpp::nodes::Value*>(obj);
        const std::string& relname = v->GetString();

        const FormData_pg_class* class_row = cat->GetClassByName(relname);
        if (class_row == nullptr) {
            if (stmt->missing_ok)
                continue;
            ereport(pgcpp::error::LogLevel::kError, "table \"" + relname + "\" does not exist");
        }

        Oid relid = class_row->oid;
        Relation rel = RelationOpen(relid);
        if (rel != nullptr) {
            RelationDropStorage(rel);
            RelationClose(rel);
        }
        cat->DeleteAttributes(relid);
        cat->DeleteClass(relid);
    }

    return tag;
}

std::string AlterTable(AlterTableStmt* stmt) {
    if (stmt == nullptr || stmt->relation == nullptr)
        return "";

    Catalog* cat = GetCatalog();
    if (cat == nullptr)
        return "";

    const std::string& relname = stmt->relation->relname;
    const FormData_pg_class* class_row = cat->GetClassByName(relname);
    if (class_row == nullptr) {
        if (stmt->missing_ok)
            return "ALTER TABLE";
        ereport(pgcpp::error::LogLevel::kError, "relation \"" + relname + "\" does not exist");
    }
    Oid relid = class_row->oid;

    for (Node* cmd_node : stmt->cmds) {
        if (cmd_node == nullptr || cmd_node->GetTag() != NodeTag::kAlterTableCmd)
            continue;
        auto* cmd = static_cast<AlterTableCmd*>(cmd_node);

        switch (cmd->subtype) {
            case pgcpp::parser::AlterTableType::kAddColumn:
            case pgcpp::parser::AlterTableType::kAddColumnRecurse: {
                if (cmd->def == nullptr || cmd->def->GetTag() != NodeTag::kColumnDef)
                    break;
                auto* coldef = static_cast<ColumnDef*>(cmd->def);
                std::string type_name = ExtractTypeName(coldef->type_name);
                Oid type_oid = typenameTypeId(type_name);
                if (type_oid == pgcpp::types::kInvalidOid) {
                    ereport(pgcpp::error::LogLevel::kError,
                            "type \"" + type_name + "\" does not exist");
                }
                int16_t attnum = class_row->relnatts + 1;
                auto* attr_row = makePallocNode<FormData_pg_attribute>();
                attr_row->attrelid = relid;
                attr_row->attname = coldef->colname;
                attr_row->atttypid = type_oid;
                attr_row->attlen = get_typlen(type_oid);
                attr_row->attnum = attnum;
                attr_row->attbyval = get_typbyval(type_oid);
                attr_row->attstorage = AttStorage::kPlain;
                attr_row->attalign = static_cast<AttAlign>(get_typalign(type_oid));
                attr_row->attnotnull = coldef->is_not_null;
                attr_row->attislocal = true;
                cat->InsertAttribute(attr_row);

                auto* mut = const_cast<FormData_pg_class*>(class_row);
                mut->relnatts = attnum;
                break;
            }
            case pgcpp::parser::AlterTableType::kDropColumn:
            case pgcpp::parser::AlterTableType::kDropColumnRecurse: {
                auto attrs = cat->GetAttributes(relid);
                bool found = false;
                for (const FormData_pg_attribute* attr : attrs) {
                    if (attr->attname == cmd->name) {
                        auto* mut = const_cast<FormData_pg_attribute*>(attr);
                        mut->attisdropped = true;
                        found = true;
                        break;
                    }
                }
                if (!found && !cmd->missing_ok) {
                    ereport(pgcpp::error::LogLevel::kError, "column \"" + cmd->name +
                                                                "\" of relation \"" + relname +
                                                                "\" does not exist");
                }
                auto* mut = const_cast<FormData_pg_class*>(class_row);
                if (mut->relnatts > 0)
                    mut->relnatts--;
                break;
            }
            default:
                // F-4f: Unsupported ALTER TABLE subcommands — fail
                // explicitly rather than silently succeeding.
                ereport(pgcpp::error::LogLevel::kError,
                        "ALTER TABLE subcommand is not supported");
                break;
        }
    }

    return "ALTER TABLE";
}

std::string RenameRelation(RenameStmt* stmt) {
    if (stmt == nullptr || stmt->relation == nullptr)
        return "";

    Catalog* cat = GetCatalog();
    if (cat == nullptr)
        return "";

    const std::string& relname = stmt->relation->relname;
    const FormData_pg_class* class_row = cat->GetClassByName(relname);
    if (class_row == nullptr) {
        if (stmt->missing_ok)
            return "ALTER TABLE";
        ereport(pgcpp::error::LogLevel::kError, "relation \"" + relname + "\" does not exist");
    }
    Oid relid = class_row->oid;

    if (stmt->subname.empty()) {
        auto* mut = const_cast<FormData_pg_class*>(class_row);
        mut->relname = stmt->newname;
        return "ALTER TABLE";
    }

    auto attrs = cat->GetAttributes(relid);
    bool found = false;
    for (const FormData_pg_attribute* attr : attrs) {
        if (attr->attname == stmt->subname) {
            auto* mut = const_cast<FormData_pg_attribute*>(attr);
            mut->attname = stmt->newname;
            found = true;
            break;
        }
    }
    if (!found) {
        ereport(pgcpp::error::LogLevel::kError,
                "column \"" + stmt->subname + "\" of relation \"" + relname + "\" does not exist");
    }
    return "ALTER TABLE";
}

std::string ExecuteTruncate(TruncateStmt* stmt) {
    if (stmt == nullptr)
        return "TRUNCATE TABLE";

    Catalog* cat = GetCatalog();
    if (cat == nullptr)
        return "TRUNCATE TABLE";

    for (Node* node : stmt->relations) {
        if (node == nullptr || node->GetTag() != NodeTag::kRangeVar)
            continue;
        auto* rv = static_cast<RangeVar*>(node);
        const FormData_pg_class* class_row = cat->GetClassByName(rv->relname);
        if (class_row == nullptr) {
            ereport(pgcpp::error::LogLevel::kError,
                    "relation \"" + rv->relname + "\" does not exist");
        }
        Oid relid = class_row->oid;
        Relation rel = RelationOpen(relid);
        if (rel != nullptr) {
            // A-6 fix: assign a NEW relfilenode instead of unlinking and
            // recreating the old one. The new (empty) storage file is
            // created BEFORE the old file is removed, so there is never a
            // window where the relation has no storage (crash-safe). This
            // mirrors PostgreSQL's TRUNCATE architecture (new relfilenode
            // per TRUNCATE). PostgreSQL queues the old relfilenode for
            // post-commit deletion; pgcpp lacks deferred delete, so the old
            // file is unlinked now — ROLLBACK safety requires deferred
            // delete + catalog undo (future work).
            auto old_srel = pgcpp::access::RelationGetSmgr(rel);

            Oid new_relfilenode = cat->AllocateOid();
            auto* mut = const_cast<FormData_pg_class*>(class_row);
            mut->relfilenode = new_relfilenode;

            // Create the new (empty) storage file first.
            RelationCreateStorage(new_relfilenode, false);

            // Now drop buffers and unlink the old storage file.
            if (old_srel != nullptr) {
                pgcpp::storage::DropRelationBuffers(old_srel->smgr_rnode.node);
                smgrdounlinkall(old_srel, false);
                rel->rd_smgr = nullptr;  // next RelationGetSmgr opens new relfilenode
            }
            RelationClose(rel);
        }
    }
    return "TRUNCATE TABLE";
}

}  // namespace pgcpp::commands

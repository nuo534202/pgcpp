// tablecmds.cpp — CREATE/ALTER/DROP TABLE / TRUNCATE implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/tablecmds.c.
// These handlers were previously inline in src/protocol/utility.cpp;
// they have been moved here so utility.cpp can be a thin dispatcher
// (matching PostgreSQL's tcop/utility.c + commands/* separation).
#include "mytoydb/commands/tablecmds.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "mytoydb/access/heapam.hpp"
#include "mytoydb/access/rel.hpp"
#include "mytoydb/catalog/catalog.hpp"
#include "mytoydb/catalog/pg_attribute.hpp"
#include "mytoydb/catalog/pg_class.hpp"
#include "mytoydb/common/containers/node.hpp"
#include "mytoydb/common/error/elog.hpp"
#include "mytoydb/common/memory/memory_context.hpp"
#include "mytoydb/parser/parse_type.hpp"
#include "mytoydb/parser/parsenodes.hpp"
#include "mytoydb/storage/smgr.hpp"
#include "mytoydb/types/builtins.hpp"

namespace mytoydb::commands {

using mytoydb::access::Relation;
using mytoydb::access::RelationClose;
using mytoydb::access::RelationCreateStorage;
using mytoydb::access::RelationDropStorage;
using mytoydb::access::RelationOpen;
using mytoydb::catalog::AttAlign;
using mytoydb::catalog::AttStorage;
using mytoydb::catalog::Catalog;
using mytoydb::catalog::FormData_pg_attribute;
using mytoydb::catalog::FormData_pg_class;
using mytoydb::catalog::GetCatalog;
using mytoydb::catalog::Oid;
using mytoydb::catalog::RelKind;
using mytoydb::catalog::RelPersistence;
using mytoydb::memory::palloc;
using mytoydb::memory::pfree;
using mytoydb::nodes::makePallocNode;
using mytoydb::nodes::NodeTag;
using mytoydb::parser::AlterTableCmd;
using mytoydb::parser::AlterTableStmt;
using mytoydb::parser::ColumnDef;
using mytoydb::parser::CreateStmt;
using mytoydb::parser::DropStmt;
using mytoydb::parser::get_typbyval;
using mytoydb::parser::get_typlen;
using mytoydb::parser::Node;
using mytoydb::parser::RangeVar;
using mytoydb::parser::RenameStmt;
using mytoydb::parser::TruncateStmt;
using mytoydb::parser::TypeName;
using mytoydb::parser::typenameTypeId;
using mytoydb::storage::smgrdounlinkall;

namespace {

// Extract the type name string from a TypeName node.
std::string ExtractTypeName(TypeName* type_name) {
    if (type_name == nullptr || type_name->names.empty())
        return "";
    Node* last = type_name->names.back();
    if (last->GetTag() == NodeTag::kString) {
        auto* v = static_cast<mytoydb::nodes::Value*>(last);
        return v->GetString();
    }
    return "";
}

// Determine the alignment for a type based on typlen.
AttAlign TypeAlignForLen(int16_t typlen) {
    if (typlen == 1)
        return AttAlign::kChar;
    if (typlen == 2)
        return AttAlign::kShort;
    if (typlen == 4)
        return AttAlign::kInt;
    if (typlen == 8 || typlen > 0)
        return AttAlign::kDouble;
    return AttAlign::kInt;  // varlena types
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
        ereport(mytoydb::error::LogLevel::kError, "relation \"" + relname + "\" already exists");
    }

    // Resolve column types.
    int16_t natts = 0;
    struct ColInfo {
        std::string name;
        Oid type_oid;
        int16_t typlen;
        bool typbyval;
    };
    std::vector<ColInfo> columns;

    for (Node* elt : stmt->table_elts) {
        if (elt == nullptr || elt->GetTag() != NodeTag::kColumnDef)
            continue;
        auto* coldef = static_cast<ColumnDef*>(elt);

        ColInfo ci;
        ci.name = coldef->colname;
        std::string type_name = ExtractTypeName(coldef->type_name);
        ci.type_oid = typenameTypeId(type_name);
        if (ci.type_oid == mytoydb::types::kInvalidOid) {
            ereport(mytoydb::error::LogLevel::kError, "type \"" + type_name + "\" does not exist");
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
        attr_row->attalign = TypeAlignForLen(ci.typlen);
        attr_row->attnotnull = false;
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
        case mytoydb::parser::ObjectType::kTable:
            tag = "DROP TABLE";
            break;
        case mytoydb::parser::ObjectType::kIndex:
            tag = "DROP INDEX";
            break;
        case mytoydb::parser::ObjectType::kView:
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
        auto* v = static_cast<mytoydb::nodes::Value*>(obj);
        const std::string& relname = v->GetString();

        const FormData_pg_class* class_row = cat->GetClassByName(relname);
        if (class_row == nullptr) {
            if (stmt->missing_ok)
                continue;
            ereport(mytoydb::error::LogLevel::kError, "table \"" + relname + "\" does not exist");
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
        ereport(mytoydb::error::LogLevel::kError, "relation \"" + relname + "\" does not exist");
    }
    Oid relid = class_row->oid;

    for (Node* cmd_node : stmt->cmds) {
        if (cmd_node == nullptr || cmd_node->GetTag() != NodeTag::kAlterTableCmd)
            continue;
        auto* cmd = static_cast<AlterTableCmd*>(cmd_node);

        switch (cmd->subtype) {
            case mytoydb::parser::AlterTableType::kAddColumn:
            case mytoydb::parser::AlterTableType::kAddColumnRecurse: {
                if (cmd->def == nullptr || cmd->def->GetTag() != NodeTag::kColumnDef)
                    break;
                auto* coldef = static_cast<ColumnDef*>(cmd->def);
                std::string type_name = ExtractTypeName(coldef->type_name);
                Oid type_oid = typenameTypeId(type_name);
                if (type_oid == mytoydb::types::kInvalidOid) {
                    ereport(mytoydb::error::LogLevel::kError,
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
                attr_row->attalign = TypeAlignForLen(get_typlen(type_oid));
                attr_row->attnotnull = false;
                attr_row->attislocal = true;
                cat->InsertAttribute(attr_row);

                auto* mut = const_cast<FormData_pg_class*>(class_row);
                mut->relnatts = attnum;
                break;
            }
            case mytoydb::parser::AlterTableType::kDropColumn:
            case mytoydb::parser::AlterTableType::kDropColumnRecurse: {
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
                    ereport(mytoydb::error::LogLevel::kError, "column \"" + cmd->name +
                                                                  "\" of relation \"" + relname +
                                                                  "\" does not exist");
                }
                auto* mut = const_cast<FormData_pg_class*>(class_row);
                if (mut->relnatts > 0)
                    mut->relnatts--;
                break;
            }
            default:
                // Other ALTER TABLE subcommands not yet supported.
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
        ereport(mytoydb::error::LogLevel::kError, "relation \"" + relname + "\" does not exist");
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
        ereport(mytoydb::error::LogLevel::kError,
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
            ereport(mytoydb::error::LogLevel::kError,
                    "relation \"" + rv->relname + "\" does not exist");
        }
        Oid relid = class_row->oid;
        Relation rel = RelationOpen(relid);
        if (rel != nullptr) {
            auto srel = mytoydb::access::RelationGetSmgr(rel);
            if (srel != nullptr) {
                smgrdounlinkall(srel, false);
                rel->rd_smgr = nullptr;
            }
            RelationCreateStorage(class_row->relfilenode, false);
            RelationClose(rel);
        }
    }
    return "TRUNCATE TABLE";
}

}  // namespace mytoydb::commands

// utility.cpp — Utility command dispatch (ProcessUtility).
//
// Converted from PostgreSQL 15's src/backend/tcop/utility.c.
//
// Dispatches non-SELECT/DML statements to their handlers, mutating the
// catalog and storage layer. CREATE TABLE handling is moved here from
// postgres.cpp; additional handlers cover DROP, ALTER, CREATE INDEX,
// TRUNCATE, VACUUM, COPY, and SET/RESET.
#include "mytoydb/protocol/utility.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "mytoydb/access/heapam.hpp"
#include "mytoydb/access/nbtpage.hpp"
#include "mytoydb/access/nbtree.hpp"
#include "mytoydb/access/rel.hpp"
#include "mytoydb/catalog/catalog.hpp"
#include "mytoydb/catalog/pg_attribute.hpp"
#include "mytoydb/catalog/pg_class.hpp"
#include "mytoydb/common/containers/node.hpp"
#include "mytoydb/common/error/elog.hpp"
#include "mytoydb/common/memory/memory_context.hpp"
#include "mytoydb/parser/parse_type.hpp"
#include "mytoydb/parser/parsenodes.hpp"
#include "mytoydb/parser/primnodes.hpp"
#include "mytoydb/storage/smgr.hpp"
#include "mytoydb/transaction/heap_tuple.hpp"
#include "mytoydb/transaction/snapshot.hpp"
#include "mytoydb/transaction/xact.hpp"
#include "mytoydb/types/builtins.hpp"
#include "mytoydb/types/datum.hpp"

namespace mytoydb::protocol {

using mytoydb::access::btbuild;
using mytoydb::access::BTKeyKind;
using mytoydb::access::heap_beginscan;
using mytoydb::access::heap_deform_tuple;
using mytoydb::access::heap_endscan;
using mytoydb::access::heap_form_tuple;
using mytoydb::access::heap_freetuple;
using mytoydb::access::heap_getnext;
using mytoydb::access::heap_insert;
using mytoydb::access::HeapScanDesc;
using mytoydb::access::Relation;
using mytoydb::access::RelationClose;
using mytoydb::access::RelationCreateStorage;
using mytoydb::access::RelationDropStorage;
using mytoydb::access::RelationOpen;
using mytoydb::access::TupleDesc;
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
using mytoydb::parser::CopyStmt;
using mytoydb::parser::CreateStmt;
using mytoydb::parser::DropStmt;
using mytoydb::parser::get_typbyval;
using mytoydb::parser::get_typlen;
using mytoydb::parser::IndexElem;
using mytoydb::parser::IndexStmt;
using mytoydb::parser::Node;
using mytoydb::parser::RangeVar;
using mytoydb::parser::RenameStmt;
using mytoydb::parser::TransactionStmt;
using mytoydb::parser::TruncateStmt;
using mytoydb::parser::TypeName;
using mytoydb::parser::typenameTypeId;
using mytoydb::parser::VacuumStmt;
using mytoydb::parser::VariableSetStmt;
using mytoydb::storage::smgrdounlinkall;
using mytoydb::transaction::BeginTransactionBlock;
using mytoydb::transaction::EndTransactionBlock;
using mytoydb::transaction::GetTransactionSnapshot;
using mytoydb::transaction::HeapTuple;
using mytoydb::transaction::Snapshot;
using mytoydb::types::Datum;
using mytoydb::types::kInt2Oid;
using mytoydb::types::kInt4Oid;
using mytoydb::types::kInt8Oid;
using mytoydb::types::kTextOid;
using mytoydb::types::kVarcharOid;

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
mytoydb::catalog::AttAlign TypeAlignForLen(int16_t typlen) {
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

// Map a type OID to the B-tree key kind. Defaults to int32.
BTKeyKind BtKeyKindForType(Oid type_oid) {
    switch (type_oid) {
        case kInt2Oid:
        case kInt4Oid:
            return BTKeyKind::kInt32;
        case kInt8Oid:
            return BTKeyKind::kInt64;
        case kTextOid:
        case kVarcharOid:
            return BTKeyKind::kText;
        default:
            return BTKeyKind::kInt32;
    }
}

// Convert a text value to a Datum based on the type OID (for COPY FROM).
Datum TextToDatum(const std::string& text, Oid type_oid) {
    switch (type_oid) {
        case kInt2Oid:
        case kInt4Oid:
            return mytoydb::types::int4_in(text.c_str());
        case kInt8Oid:
            return mytoydb::types::int8_in(text.c_str());
        case kTextOid:
        case kVarcharOid:
            return mytoydb::types::text_in(text.c_str());
        default:
            return mytoydb::types::int4_in(text.c_str());
    }
}

// --- Statement handlers ---

// ProcessTransactionStmt — BEGIN / COMMIT / ROLLBACK.
std::string ProcessTransactionStmt(TransactionStmt* stmt) {
    switch (stmt->kind) {
        case TransactionStmt::Kind::kBegin:
        case TransactionStmt::Kind::kStart:
            BeginTransactionBlock();
            return "BEGIN";
        case TransactionStmt::Kind::kCommit:
            EndTransactionBlock();
            return "COMMIT";
        case TransactionStmt::Kind::kRollback:
            mytoydb::transaction::AbortTransactionBlock();
            return "ROLLBACK";
        default:
            // SAVEPOINT / RELEASE / ROLLBACK TO — not yet supported.
            return "";
    }
}

// ProcessCreateTable — execute a CREATE TABLE statement.
// Creates the pg_class and pg_attribute entries and the physical storage file.
std::string ProcessCreateTable(CreateStmt* stmt) {
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

// ProcessDropStmt — DROP TABLE / DROP INDEX / DROP VIEW.
std::string ProcessDropStmt(DropStmt* stmt) {
    if (stmt == nullptr)
        return "";

    // Map remove_type to the command tag.
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

    // objects is a flat list of String nodes (relation names). See grammar:
    // any_name_list flattens qualified names; each String is treated as a
    // separate relation to drop.
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
        // Drop physical storage.
        Relation rel = RelationOpen(relid);
        if (rel != nullptr) {
            RelationDropStorage(rel);
            RelationClose(rel);
        }
        // Drop catalog entries.
        cat->DeleteAttributes(relid);
        cat->DeleteClass(relid);
    }

    return tag;
}

// ProcessAlterTableStmt — ALTER TABLE ADD/DROP/RENAME COLUMN.
std::string ProcessAlterTableStmt(AlterTableStmt* stmt) {
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

                // Increment relnatts in pg_class (const_cast: the catalog
                // stores mutable pointers; the const is on the lookup API).
                auto* mut = const_cast<FormData_pg_class*>(class_row);
                mut->relnatts = attnum;
                break;
            }
            case mytoydb::parser::AlterTableType::kDropColumn:
            case mytoydb::parser::AlterTableType::kDropColumnRecurse: {
                // Find the attribute by name and mark it dropped.
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

// ProcessRenameStmt — ALTER TABLE RENAME COLUMN / RENAME TABLE.
// ALTER TABLE RENAME produces a RenameStmt (not an AlterTableStmt).
std::string ProcessRenameStmt(mytoydb::parser::RenameStmt* stmt) {
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
        // RENAME TABLE (rename the relation itself).
        auto* mut = const_cast<FormData_pg_class*>(class_row);
        mut->relname = stmt->newname;
        return "ALTER TABLE";
    }

    // RENAME COLUMN: find the attribute by old name and rename it.
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

// ProcessIndexStmt — CREATE INDEX.
std::string ProcessIndexStmt(IndexStmt* stmt) {
    if (stmt == nullptr || stmt->relation == nullptr)
        return "";

    Catalog* cat = GetCatalog();
    if (cat == nullptr)
        return "";

    const std::string& heapname = stmt->relation->relname;
    const FormData_pg_class* heap_row = cat->GetClassByName(heapname);
    if (heap_row == nullptr) {
        ereport(mytoydb::error::LogLevel::kError, "relation \"" + heapname + "\" does not exist");
    }
    Oid heap_oid = heap_row->oid;

    // Determine the index key kind from the first index column.
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

    // Create pg_class entry for the index.
    std::string idxname = stmt->idxname;
    if (idxname.empty())
        idxname = heapname + "_idx";

    if (cat->GetClassByName(idxname) != nullptr) {
        if (stmt->if_not_exists)
            return "CREATE INDEX";
        ereport(mytoydb::error::LogLevel::kError, "relation \"" + idxname + "\" already exists");
    }

    auto* class_row = makePallocNode<FormData_pg_class>();
    class_row->relname = idxname;
    class_row->relnamespace = 2200;
    class_row->relkind = RelKind::kIndex;
    class_row->relpersistence = RelPersistence::kPermanent;
    class_row->relnatts = static_cast<int16_t>(stmt->index_params.size());
    class_row->relispopulated = true;
    Oid index_oid = cat->InsertClass(class_row);
    class_row->relfilenode = index_oid;

    // Create pg_attribute entries for index columns.
    int16_t attnum = 1;
    for (Node* node : stmt->index_params) {
        if (node == nullptr || node->GetTag() != NodeTag::kIndexElem)
            continue;
        auto* elem = static_cast<IndexElem*>(node);
        // Look up the column type from the heap's attributes.
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
        attr_row->attalign = TypeAlignForLen(col_len);
        attr_row->attnotnull = false;
        attr_row->attislocal = true;
        cat->InsertAttribute(attr_row);
        attnum++;
    }

    // Create physical storage and initialize the empty B-tree.
    RelationCreateStorage(index_oid, false);
    Relation index_rel = RelationOpen(index_oid);
    if (index_rel != nullptr) {
        btbuild(index_rel, key_kind);
        RelationClose(index_rel);
    }

    return "CREATE INDEX";
}

// ProcessTruncateStmt — TRUNCATE TABLE.
std::string ProcessTruncateStmt(TruncateStmt* stmt) {
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
            // TRUNCATE must physically delete the file before recreating it.
            // RelationDropStorage only closes the smgr handle; it does NOT
            // unlink the file. Use smgrdounlinkall to delete the fork files,
            // then recreate fresh storage with the same relfilenode.
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

// ProcessVacuumStmt — VACUUM / ANALYZE (simplified: no-op).
std::string ProcessVacuumStmt(VacuumStmt* stmt) {
    if (stmt == nullptr)
        return "VACUUM";
    return stmt->is_vacuumcmd ? "VACUUM" : "ANALYZE";
}

// ProcessVariableSetStmt — SET / RESET (simplified: no-op).
std::string ProcessVariableSetStmt(VariableSetStmt* stmt) {
    if (stmt == nullptr)
        return "SET";
    switch (stmt->kind) {
        case VariableSetStmt::Kind::kReset:
        case VariableSetStmt::Kind::kResetAll:
            return "RESET";
        default:
            return "SET";
    }
}

// ProcessCopyStmt — COPY (simplified: text format only).
std::string ProcessCopyStmt(CopyStmt* stmt) {
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
    TupleDesc tupdesc = rel->rd_att;
    int natts = tupdesc->natts;

    // Raw arrays are required because std::vector<bool> does not expose a
    // contiguous bool* (it is bit-packed). heap_form_tuple / heap_deform_tuple
    // require bool*. Allocate once per COPY; natts is constant for the relation.
    Datum* values = static_cast<Datum*>(palloc(sizeof(Datum) * natts));
    bool* isnull = static_cast<bool*>(palloc(sizeof(bool) * natts));

    int64_t row_count = 0;
    if (stmt->is_from) {
        // COPY FROM: read text lines from file and insert.
        std::ifstream in(stmt->filename);
        if (!in.is_open()) {
            pfree(isnull);
            pfree(values);
            RelationClose(rel);
            ereport(mytoydb::error::LogLevel::kError,
                    "could not open file \"" + stmt->filename + "\"");
        }
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty())
                continue;
            for (int i = 0; i < natts; ++i) {
                values[i] = 0;
                isnull[i] = false;
            }
            // Split on tab.
            std::size_t pos = 0;
            for (int i = 0; i < natts; ++i) {
                std::size_t tab = line.find('\t', pos);
                std::string field =
                    (tab == std::string::npos) ? line.substr(pos) : line.substr(pos, tab - pos);
                if (field.empty()) {
                    isnull[i] = true;
                } else {
                    values[i] = TextToDatum(field, tupdesc->attrs[i].atttypid);
                }
                if (tab == std::string::npos)
                    break;
                pos = tab + 1;
            }
            HeapTuple tup = heap_form_tuple(tupdesc, values, isnull);
            heap_insert(rel, tup);
            heap_freetuple(tup);
            row_count++;
        }
    } else {
        // COPY TO: scan relation and write text lines to file.
        std::ofstream out(stmt->filename);
        if (!out.is_open()) {
            pfree(isnull);
            pfree(values);
            RelationClose(rel);
            ereport(mytoydb::error::LogLevel::kError,
                    "could not open file \"" + stmt->filename + "\"");
        }
        Snapshot snap = GetTransactionSnapshot();
        HeapScanDesc scan = heap_beginscan(rel, snap);
        HeapTuple tup;
        while ((tup = heap_getnext(scan)) != nullptr) {
            heap_deform_tuple(tup, tupdesc, values, isnull);
            std::string line;
            for (int i = 0; i < natts; ++i) {
                if (i > 0)
                    line.push_back('\t');
                if (isnull[i]) {
                    // empty field represents NULL
                } else {
                    line += mytoydb::types::int4_out(
                        values[i]);  // placeholder; real impl uses type out
                }
            }
            out << line << "\n";
            row_count++;
        }
        heap_endscan(scan);
    }

    pfree(isnull);
    pfree(values);
    RelationClose(rel);
    return "COPY " + std::to_string(row_count);
}

}  // namespace

// --- Public API ---

std::string ProcessUtility(Node* stmt, OutputSink* /*sink*/) {
    if (stmt == nullptr)
        return "";

    switch (stmt->GetTag()) {
        case NodeTag::kTransactionStmt:
            return ProcessTransactionStmt(static_cast<TransactionStmt*>(stmt));
        case NodeTag::kCreateStmt:
            return ProcessCreateTable(static_cast<CreateStmt*>(stmt));
        case NodeTag::kDropStmt:
            return ProcessDropStmt(static_cast<DropStmt*>(stmt));
        case NodeTag::kAlterTableStmt:
            return ProcessAlterTableStmt(static_cast<AlterTableStmt*>(stmt));
        case NodeTag::kRenameStmt:
            return ProcessRenameStmt(static_cast<RenameStmt*>(stmt));
        case NodeTag::kIndexStmt:
            return ProcessIndexStmt(static_cast<IndexStmt*>(stmt));
        case NodeTag::kTruncateStmt:
            return ProcessTruncateStmt(static_cast<TruncateStmt*>(stmt));
        case NodeTag::kVacuumStmt:
            return ProcessVacuumStmt(static_cast<VacuumStmt*>(stmt));
        case NodeTag::kVariableSetStmt:
            return ProcessVariableSetStmt(static_cast<VariableSetStmt*>(stmt));
        case NodeTag::kCopyStmt:
            return ProcessCopyStmt(static_cast<CopyStmt*>(stmt));
        default:
            return "";
    }
}

std::string CreateCommandTag(Node* stmt) {
    if (stmt == nullptr)
        return "";
    switch (stmt->GetTag()) {
        case NodeTag::kTransactionStmt: {
            auto* ts = static_cast<TransactionStmt*>(stmt);
            switch (ts->kind) {
                case TransactionStmt::Kind::kBegin:
                case TransactionStmt::Kind::kStart:
                    return "BEGIN";
                case TransactionStmt::Kind::kCommit:
                    return "COMMIT";
                case TransactionStmt::Kind::kRollback:
                    return "ROLLBACK";
                default:
                    return "BEGIN";
            }
        }
        case NodeTag::kCreateStmt:
            return "CREATE TABLE";
        case NodeTag::kDropStmt: {
            auto* d = static_cast<DropStmt*>(stmt);
            switch (d->remove_type) {
                case mytoydb::parser::ObjectType::kTable:
                    return "DROP TABLE";
                case mytoydb::parser::ObjectType::kIndex:
                    return "DROP INDEX";
                case mytoydb::parser::ObjectType::kView:
                    return "DROP VIEW";
                default:
                    return "DROP";
            }
        }
        case NodeTag::kAlterTableStmt:
            return "ALTER TABLE";
        case NodeTag::kRenameStmt:
            return "ALTER TABLE";
        case NodeTag::kIndexStmt:
            return "CREATE INDEX";
        case NodeTag::kTruncateStmt:
            return "TRUNCATE TABLE";
        case NodeTag::kVacuumStmt: {
            auto* v = static_cast<VacuumStmt*>(stmt);
            return v->is_vacuumcmd ? "VACUUM" : "ANALYZE";
        }
        case NodeTag::kCopyStmt:
            return "COPY";
        case NodeTag::kVariableSetStmt: {
            auto* s = static_cast<VariableSetStmt*>(stmt);
            return (s->kind == VariableSetStmt::Kind::kReset ||
                    s->kind == VariableSetStmt::Kind::kResetAll)
                       ? "RESET"
                       : "SET";
        }
        default:
            return "";
    }
}

bool UtilityReturnsTuples(Node* /*stmt*/) {
    return false;
}

}  // namespace mytoydb::protocol

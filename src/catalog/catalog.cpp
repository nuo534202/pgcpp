// catalog.cpp — implementation of the in-memory system catalog.
//
// Converts PostgreSQL's catalog/indexing.c + catalog/heap.c catalog-tuple
// helpers to C++20. Row storage is a std::vector of palloc-allocated pointers,
// preserving PostgreSQL's "rows live in a long-lived memory context" model.

#include "mytoydb/catalog/catalog.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "mytoydb/catalog/pg_aggregate.h"
#include "mytoydb/catalog/pg_attribute.h"
#include "mytoydb/catalog/pg_cast.h"
#include "mytoydb/catalog/pg_class.h"
#include "mytoydb/catalog/pg_collation.h"
#include "mytoydb/catalog/pg_operator.h"
#include "mytoydb/catalog/pg_proc.h"
#include "mytoydb/catalog/pg_type.h"
#include "mytoydb/common/error/elog.h"
#include "mytoydb/common/memory/memory_context.h"

namespace mytoydb::catalog {

namespace {

// Process-wide catalog pointer. Defaults to nullptr; tests set it via
// SetCatalog. In a full implementation this would be initialized during
// bootstrap (Phase 12).
Catalog* g_catalog = nullptr;

}  // namespace

// --- Catalog: pg_class ---

Oid Catalog::InsertClass(FormData_pg_class* row) {
    if (row == nullptr) {
        ereport(error::LogLevel::kError, "Catalog::InsertClass: row is null");
    }
    if (row->oid == kInvalidOid) {
        row->oid = AllocateOid();
    }
    // Reject duplicate OIDs.
    for (const auto* existing : pg_class_rows_) {
        if (existing->oid == row->oid) {
            ereport(error::LogLevel::kError, "Catalog::InsertClass: duplicate OID");
        }
    }
    pg_class_rows_.push_back(row);
    return row->oid;
}

const FormData_pg_class* Catalog::GetClassByOid(Oid oid) const {
    for (const auto* row : pg_class_rows_) {
        if (row->oid == oid) {
            return row;
        }
    }
    return nullptr;
}

const FormData_pg_class* Catalog::GetClassByName(const std::string& name) const {
    for (const auto* row : pg_class_rows_) {
        if (row->relname == name) {
            return row;
        }
    }
    return nullptr;
}

bool Catalog::UpdateClass(Oid oid, const FormData_pg_class* new_row) {
    for (auto*& row : pg_class_rows_) {
        if (row->oid == oid) {
            // Copy fields from new_row into the existing row (in-place update,
            // preserving the row pointer identity that SysCache may pin).
            row->relname = new_row->relname;
            row->relnamespace = new_row->relnamespace;
            row->reltype = new_row->reltype;
            row->reloftype = new_row->reloftype;
            row->relowner = new_row->relowner;
            row->relam = new_row->relam;
            row->relfilenode = new_row->relfilenode;
            row->reltablespace = new_row->reltablespace;
            row->relpages = new_row->relpages;
            row->reltuples = new_row->reltuples;
            row->reltoastrelid = new_row->reltoastrelid;
            row->relhasindex = new_row->relhasindex;
            row->relisshared = new_row->relisshared;
            row->relpersistence = new_row->relpersistence;
            row->relkind = new_row->relkind;
            row->relnatts = new_row->relnatts;
            row->relchecks = new_row->relchecks;
            row->relhasrules = new_row->relhasrules;
            row->relhastriggers = new_row->relhastriggers;
            row->relrowsecurity = new_row->relrowsecurity;
            row->relforcerowsecurity = new_row->relforcerowsecurity;
            row->relispopulated = new_row->relispopulated;
            row->relreplident = new_row->relreplident;
            row->relispartition = new_row->relispartition;
            row->relfrozenxid = new_row->relfrozenxid;
            row->relminmxid = new_row->relminmxid;
            return true;
        }
    }
    return false;
}

bool Catalog::DeleteClass(Oid oid) {
    auto it = std::find_if(pg_class_rows_.begin(), pg_class_rows_.end(),
                           [oid](const FormData_pg_class* r) { return r->oid == oid; });
    if (it == pg_class_rows_.end()) {
        return false;
    }
    pg_class_rows_.erase(it);
    return true;
}

// --- Catalog: pg_attribute ---

void Catalog::InsertAttribute(FormData_pg_attribute* row) {
    if (row == nullptr) {
        ereport(error::LogLevel::kError, "Catalog::InsertAttribute: row is null");
    }
    pg_attribute_rows_.push_back(row);
}

const FormData_pg_attribute* Catalog::GetAttribute(Oid relid, int16_t attnum) const {
    for (const auto* row : pg_attribute_rows_) {
        if (row->attrelid == relid && row->attnum == attnum) {
            return row;
        }
    }
    return nullptr;
}

std::vector<const FormData_pg_attribute*> Catalog::GetAttributes(Oid relid) const {
    std::vector<const FormData_pg_attribute*> result;
    for (const auto* row : pg_attribute_rows_) {
        if (row->attrelid == relid) {
            result.push_back(row);
        }
    }
    // Sort by attnum (PostgreSQL keeps attributes in attnum order).
    std::sort(result.begin(), result.end(),
              [](const FormData_pg_attribute* a, const FormData_pg_attribute* b) {
                  return a->attnum < b->attnum;
              });
    return result;
}

std::size_t Catalog::DeleteAttributes(Oid relid) {
    auto original_size = pg_attribute_rows_.size();
    pg_attribute_rows_.erase(
        std::remove_if(pg_attribute_rows_.begin(), pg_attribute_rows_.end(),
                       [relid](const FormData_pg_attribute* r) { return r->attrelid == relid; }),
        pg_attribute_rows_.end());
    return original_size - pg_attribute_rows_.size();
}

// --- Catalog: pg_type ---

Oid Catalog::InsertType(FormData_pg_type* row) {
    if (row == nullptr) {
        ereport(error::LogLevel::kError, "Catalog::InsertType: row is null");
    }
    if (row->oid == kInvalidOid) {
        row->oid = AllocateOid();
    }
    for (const auto* existing : pg_type_rows_) {
        if (existing->oid == row->oid) {
            ereport(error::LogLevel::kError, "Catalog::InsertType: duplicate OID");
        }
    }
    pg_type_rows_.push_back(row);
    return row->oid;
}

const FormData_pg_type* Catalog::GetTypeByOid(Oid oid) const {
    for (const auto* row : pg_type_rows_) {
        if (row->oid == oid) {
            return row;
        }
    }
    return nullptr;
}

const FormData_pg_type* Catalog::GetTypeByName(const std::string& name) const {
    for (const auto* row : pg_type_rows_) {
        if (row->typname == name) {
            return row;
        }
    }
    return nullptr;
}

// --- Catalog: pg_operator ---

Oid Catalog::InsertOperator(FormData_pg_operator* row) {
    if (row == nullptr) {
        ereport(error::LogLevel::kError, "Catalog::InsertOperator: row is null");
    }
    if (row->oid == kInvalidOid) {
        row->oid = AllocateOid();
    }
    pg_operator_rows_.push_back(row);
    return row->oid;
}

const FormData_pg_operator* Catalog::GetOperatorByOid(Oid oid) const {
    for (const auto* row : pg_operator_rows_) {
        if (row->oid == oid) return row;
    }
    return nullptr;
}

std::vector<const FormData_pg_operator*>
Catalog::GetOperatorsByName(const std::string& name) const {
    std::vector<const FormData_pg_operator*> result;
    for (const auto* row : pg_operator_rows_) {
        if (row->oprname == name) result.push_back(row);
    }
    return result;
}

const FormData_pg_operator*
Catalog::GetOperator(const std::string& name, Oid left_type, Oid right_type) const {
    for (const auto* row : pg_operator_rows_) {
        if (row->oprname == name &&
            row->oprleft == left_type &&
            row->oprright == right_type) {
            return row;
        }
    }
    return nullptr;
}

// --- Catalog: pg_proc ---

Oid Catalog::InsertProc(FormData_pg_proc* row) {
    if (row == nullptr) {
        ereport(error::LogLevel::kError, "Catalog::InsertProc: row is null");
    }
    if (row->oid == kInvalidOid) {
        row->oid = AllocateOid();
    }
    // Compute pronargs from proargtypes if not set.
    if (row->pronargs == 0 && !row->proargtypes.empty()) {
        row->pronargs = static_cast<int16_t>(row->proargtypes.size());
    }
    pg_proc_rows_.push_back(row);
    return row->oid;
}

const FormData_pg_proc* Catalog::GetProcByOid(Oid oid) const {
    for (const auto* row : pg_proc_rows_) {
        if (row->oid == oid) return row;
    }
    return nullptr;
}

std::vector<const FormData_pg_proc*>
Catalog::GetProcsByName(const std::string& name) const {
    std::vector<const FormData_pg_proc*> result;
    for (const auto* row : pg_proc_rows_) {
        if (row->proname == name) result.push_back(row);
    }
    return result;
}

// --- Catalog: pg_cast ---

Oid Catalog::InsertCast(FormData_pg_cast* row) {
    if (row == nullptr) {
        ereport(error::LogLevel::kError, "Catalog::InsertCast: row is null");
    }
    if (row->oid == kInvalidOid) {
        row->oid = AllocateOid();
    }
    pg_cast_rows_.push_back(row);
    return row->oid;
}

const FormData_pg_cast*
Catalog::GetCast(Oid source_type, Oid target_type) const {
    for (const auto* row : pg_cast_rows_) {
        if (row->castsource == source_type && row->casttarget == target_type) {
            return row;
        }
    }
    return nullptr;
}

std::vector<const FormData_pg_cast*>
Catalog::GetCastsBySource(Oid source_type) const {
    std::vector<const FormData_pg_cast*> result;
    for (const auto* row : pg_cast_rows_) {
        if (row->castsource == source_type) result.push_back(row);
    }
    return result;
}

// --- Catalog: pg_aggregate ---

void Catalog::InsertAggregate(FormData_pg_aggregate* row) {
    if (row == nullptr) {
        ereport(error::LogLevel::kError, "Catalog::InsertAggregate: row is null");
    }
    pg_aggregate_rows_.push_back(row);
}

const FormData_pg_aggregate* Catalog::GetAggregate(Oid aggfnoid) const {
    for (const auto* row : pg_aggregate_rows_) {
        if (row->aggfnoid == aggfnoid) return row;
    }
    return nullptr;
}

// --- Catalog: pg_collation ---

Oid Catalog::InsertCollation(FormData_pg_collation* row) {
    if (row == nullptr) {
        ereport(error::LogLevel::kError, "Catalog::InsertCollation: row is null");
    }
    if (row->oid == kInvalidOid) {
        row->oid = AllocateOid();
    }
    pg_collation_rows_.push_back(row);
    return row->oid;
}

const FormData_pg_collation* Catalog::GetCollationByOid(Oid oid) const {
    for (const auto* row : pg_collation_rows_) {
        if (row->oid == oid) return row;
    }
    return nullptr;
}

const FormData_pg_collation*
Catalog::GetCollationByName(const std::string& name) const {
    for (const auto* row : pg_collation_rows_) {
        if (row->collname == name) return row;
    }
    return nullptr;
}

// --- Catalog: OID assignment ---

Oid Catalog::AllocateOid() {
    return next_oid_++;
}

// --- Global accessors ---

Catalog* GetCatalog() {
    return g_catalog;
}

void SetCatalog(Catalog* catalog) {
    g_catalog = catalog;
}

// --- CatalogTuple* API ---

Oid CatalogTupleInsert(FormData_pg_class* row) {
    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        ereport(error::LogLevel::kError, "CatalogTupleInsert: catalog not initialized");
    }
    return cat->InsertClass(row);
}

bool CatalogTupleUpdate(Oid oid, const FormData_pg_class* new_row) {
    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        ereport(error::LogLevel::kError, "CatalogTupleUpdate: catalog not initialized");
    }
    return cat->UpdateClass(oid, new_row);
}

bool CatalogTupleDelete(Oid oid) {
    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        ereport(error::LogLevel::kError, "CatalogTupleDelete: catalog not initialized");
    }
    return cat->DeleteClass(oid);
}

}  // namespace mytoydb::catalog

// catalog.cpp — implementation of the in-memory system catalog.
//
// Converts PostgreSQL's catalog/indexing.c + catalog/heap.c catalog-tuple
// helpers to C++20. Row storage is a std::vector of palloc-allocated pointers,
// preserving PostgreSQL's "rows live in a long-lived memory context" model.

#include "catalog/catalog.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <utility>

#include "catalog/pg_aggregate.hpp"
#include "catalog/pg_am.hpp"
#include "catalog/pg_attrdef.hpp"
#include "catalog/pg_attribute.hpp"
#include "catalog/pg_cast.hpp"
#include "catalog/pg_class.hpp"
#include "catalog/pg_collation.hpp"
#include "catalog/pg_constraint.hpp"
#include "catalog/pg_database.hpp"
#include "catalog/pg_depend.hpp"
#include "catalog/pg_index.hpp"
#include "catalog/pg_inherits.hpp"
#include "catalog/pg_namespace.hpp"
#include "catalog/pg_operator.hpp"
#include "catalog/pg_proc.hpp"
#include "catalog/pg_rewrite.hpp"
#include "catalog/pg_statistic.hpp"
#include "catalog/pg_tablespace.hpp"
#include "catalog/pg_trigger.hpp"
#include "catalog/pg_type.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/memory_context.hpp"

namespace pgcpp::catalog {

namespace {

// Process-wide catalog pointer. Defaults to nullptr; tests set it via
// SetCatalog. In a full implementation this would be initialized during
// bootstrap (Phase 12).
Catalog* g_catalog = nullptr;

}  // namespace

// --- Catalog: pg_class ---

Oid Catalog::InsertClass(FormData_pg_class* row) {
    PreWrite();
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

std::vector<const FormData_pg_class*> Catalog::GetAllClasses() const {
    std::vector<const FormData_pg_class*> result;
    result.reserve(pg_class_rows_.size());
    for (const auto* row : pg_class_rows_)
        result.push_back(row);
    return result;
}

bool Catalog::UpdateClass(Oid oid, const FormData_pg_class* new_row) {
    PreWrite();
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
    PreWrite();
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
    PreWrite();
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
    PreWrite();
    auto original_size = pg_attribute_rows_.size();
    pg_attribute_rows_.erase(
        std::remove_if(pg_attribute_rows_.begin(), pg_attribute_rows_.end(),
                       [relid](const FormData_pg_attribute* r) { return r->attrelid == relid; }),
        pg_attribute_rows_.end());
    return original_size - pg_attribute_rows_.size();
}

// --- Catalog: pg_type ---

Oid Catalog::InsertType(FormData_pg_type* row) {
    PreWrite();
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
    PreWrite();
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
        if (row->oid == oid)
            return row;
    }
    return nullptr;
}

std::vector<const FormData_pg_operator*> Catalog::GetOperatorsByName(
    const std::string& name) const {
    std::vector<const FormData_pg_operator*> result;
    for (const auto* row : pg_operator_rows_) {
        if (row->oprname == name)
            result.push_back(row);
    }
    return result;
}

const FormData_pg_operator* Catalog::GetOperator(const std::string& name, Oid left_type,
                                                 Oid right_type) const {
    for (const auto* row : pg_operator_rows_) {
        if (row->oprname == name && row->oprleft == left_type && row->oprright == right_type) {
            return row;
        }
    }
    return nullptr;
}

// --- Catalog: pg_proc ---

Oid Catalog::InsertProc(FormData_pg_proc* row) {
    PreWrite();
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
        if (row->oid == oid)
            return row;
    }
    return nullptr;
}

std::vector<const FormData_pg_proc*> Catalog::GetProcsByName(const std::string& name) const {
    std::vector<const FormData_pg_proc*> result;
    for (const auto* row : pg_proc_rows_) {
        if (row->proname == name)
            result.push_back(row);
    }
    return result;
}

// --- Catalog: pg_cast ---

Oid Catalog::InsertCast(FormData_pg_cast* row) {
    PreWrite();
    if (row == nullptr) {
        ereport(error::LogLevel::kError, "Catalog::InsertCast: row is null");
    }
    if (row->oid == kInvalidOid) {
        row->oid = AllocateOid();
    }
    pg_cast_rows_.push_back(row);
    return row->oid;
}

const FormData_pg_cast* Catalog::GetCast(Oid source_type, Oid target_type) const {
    for (const auto* row : pg_cast_rows_) {
        if (row->castsource == source_type && row->casttarget == target_type) {
            return row;
        }
    }
    return nullptr;
}

std::vector<const FormData_pg_cast*> Catalog::GetCastsBySource(Oid source_type) const {
    std::vector<const FormData_pg_cast*> result;
    for (const auto* row : pg_cast_rows_) {
        if (row->castsource == source_type)
            result.push_back(row);
    }
    return result;
}

// --- Catalog: pg_aggregate ---

void Catalog::InsertAggregate(FormData_pg_aggregate* row) {
    PreWrite();
    if (row == nullptr) {
        ereport(error::LogLevel::kError, "Catalog::InsertAggregate: row is null");
    }
    pg_aggregate_rows_.push_back(row);
}

const FormData_pg_aggregate* Catalog::GetAggregate(Oid aggfnoid) const {
    for (const auto* row : pg_aggregate_rows_) {
        if (row->aggfnoid == aggfnoid)
            return row;
    }
    return nullptr;
}

// --- Catalog: pg_collation ---

Oid Catalog::InsertCollation(FormData_pg_collation* row) {
    PreWrite();
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
        if (row->oid == oid)
            return row;
    }
    return nullptr;
}

const FormData_pg_collation* Catalog::GetCollationByName(const std::string& name) const {
    for (const auto* row : pg_collation_rows_) {
        if (row->collname == name)
            return row;
    }
    return nullptr;
}

// --- Catalog: OID assignment ---

Oid Catalog::AllocateOid() {
    return next_oid_++;
}

// --- Catalog: pg_namespace ---

Oid Catalog::InsertNamespace(FormData_pg_namespace* row) {
    PreWrite();
    if (row == nullptr) {
        ereport(error::LogLevel::kError, "Catalog::InsertNamespace: row is null");
    }
    if (row->oid == kInvalidOid) {
        row->oid = AllocateOid();
    }
    pg_namespace_rows_.push_back(row);
    return row->oid;
}

const FormData_pg_namespace* Catalog::GetNamespaceByOid(Oid oid) const {
    for (const auto* row : pg_namespace_rows_) {
        if (row->oid == oid)
            return row;
    }
    return nullptr;
}

const FormData_pg_namespace* Catalog::GetNamespaceByName(const std::string& name) const {
    for (const auto* row : pg_namespace_rows_) {
        if (row->nspname == name)
            return row;
    }
    return nullptr;
}

bool Catalog::DeleteNamespace(Oid oid) {
    PreWrite();
    auto it = std::find_if(pg_namespace_rows_.begin(), pg_namespace_rows_.end(),
                           [oid](const FormData_pg_namespace* r) { return r->oid == oid; });
    if (it == pg_namespace_rows_.end())
        return false;
    pg_namespace_rows_.erase(it);
    return true;
}

// --- Catalog: pg_database ---

Oid Catalog::InsertDatabase(FormData_pg_database* row) {
    PreWrite();
    if (row == nullptr) {
        ereport(error::LogLevel::kError, "Catalog::InsertDatabase: row is null");
    }
    if (row->oid == kInvalidOid) {
        row->oid = AllocateOid();
    }
    pg_database_rows_.push_back(row);
    return row->oid;
}

const FormData_pg_database* Catalog::GetDatabaseByOid(Oid oid) const {
    for (const auto* row : pg_database_rows_) {
        if (row->oid == oid)
            return row;
    }
    return nullptr;
}

const FormData_pg_database* Catalog::GetDatabaseByName(const std::string& name) const {
    for (const auto* row : pg_database_rows_) {
        if (row->datname == name)
            return row;
    }
    return nullptr;
}

bool Catalog::DeleteDatabase(Oid oid) {
    PreWrite();
    auto it = std::find_if(pg_database_rows_.begin(), pg_database_rows_.end(),
                           [oid](const FormData_pg_database* r) { return r->oid == oid; });
    if (it == pg_database_rows_.end())
        return false;
    pg_database_rows_.erase(it);
    return true;
}

// --- Catalog: pg_index ---

Oid Catalog::InsertIndex(FormData_pg_index* row) {
    PreWrite();
    if (row == nullptr) {
        ereport(error::LogLevel::kError, "Catalog::InsertIndex: row is null");
    }
    // pg_index is keyed by indexrelid; if not set, allocate one.
    if (row->indexrelid == kInvalidOid) {
        row->indexrelid = AllocateOid();
    }
    pg_index_rows_.push_back(row);
    return row->indexrelid;
}

const FormData_pg_index* Catalog::GetIndexByOid(Oid indexrelid) const {
    for (const auto* row : pg_index_rows_) {
        if (row->indexrelid == indexrelid)
            return row;
    }
    return nullptr;
}

std::vector<const FormData_pg_index*> Catalog::GetIndexesByRelid(Oid indrelid) const {
    std::vector<const FormData_pg_index*> result;
    for (const auto* row : pg_index_rows_) {
        if (row->indrelid == indrelid)
            result.push_back(row);
    }
    return result;
}

bool Catalog::DeleteIndex(Oid indexrelid) {
    PreWrite();
    auto it = std::find_if(
        pg_index_rows_.begin(), pg_index_rows_.end(),
        [indexrelid](const FormData_pg_index* r) { return r->indexrelid == indexrelid; });
    if (it == pg_index_rows_.end())
        return false;
    pg_index_rows_.erase(it);
    return true;
}

std::size_t Catalog::DeleteIndexesForRelid(Oid indrelid) {
    PreWrite();
    auto original_size = pg_index_rows_.size();
    pg_index_rows_.erase(
        std::remove_if(pg_index_rows_.begin(), pg_index_rows_.end(),
                       [indrelid](const FormData_pg_index* r) { return r->indrelid == indrelid; }),
        pg_index_rows_.end());
    return original_size - pg_index_rows_.size();
}

// --- Catalog: pg_constraint ---

Oid Catalog::InsertConstraint(FormData_pg_constraint* row) {
    PreWrite();
    if (row == nullptr) {
        ereport(error::LogLevel::kError, "Catalog::InsertConstraint: row is null");
    }
    if (row->oid == kInvalidOid) {
        row->oid = AllocateOid();
    }
    pg_constraint_rows_.push_back(row);
    return row->oid;
}

const FormData_pg_constraint* Catalog::GetConstraintByOid(Oid oid) const {
    for (const auto* row : pg_constraint_rows_) {
        if (row->oid == oid)
            return row;
    }
    return nullptr;
}

std::vector<const FormData_pg_constraint*> Catalog::GetConstraintsByRelid(Oid conrelid) const {
    std::vector<const FormData_pg_constraint*> result;
    for (const auto* row : pg_constraint_rows_) {
        if (row->conrelid == conrelid)
            result.push_back(row);
    }
    return result;
}

bool Catalog::DeleteConstraint(Oid oid) {
    PreWrite();
    auto it = std::find_if(pg_constraint_rows_.begin(), pg_constraint_rows_.end(),
                           [oid](const FormData_pg_constraint* r) { return r->oid == oid; });
    if (it == pg_constraint_rows_.end())
        return false;
    pg_constraint_rows_.erase(it);
    return true;
}

std::size_t Catalog::DeleteConstraintsForRelid(Oid conrelid) {
    PreWrite();
    auto original_size = pg_constraint_rows_.size();
    pg_constraint_rows_.erase(std::remove_if(pg_constraint_rows_.begin(), pg_constraint_rows_.end(),
                                             [conrelid](const FormData_pg_constraint* r) {
                                                 return r->conrelid == conrelid;
                                             }),
                              pg_constraint_rows_.end());
    return original_size - pg_constraint_rows_.size();
}

// --- Catalog: pg_attrdef ---

Oid Catalog::InsertAttrdef(FormData_pg_attrdef* row) {
    PreWrite();
    if (row == nullptr) {
        ereport(error::LogLevel::kError, "Catalog::InsertAttrdef: row is null");
    }
    if (row->oid == kInvalidOid) {
        row->oid = AllocateOid();
    }
    pg_attrdef_rows_.push_back(row);
    return row->oid;
}

const FormData_pg_attrdef* Catalog::GetAttrdef(Oid adrelid, int16_t adnum) const {
    for (const auto* row : pg_attrdef_rows_) {
        if (row->adrelid == adrelid && row->adnum == adnum)
            return row;
    }
    return nullptr;
}

std::vector<const FormData_pg_attrdef*> Catalog::GetAttrdefsByRelid(Oid adrelid) const {
    std::vector<const FormData_pg_attrdef*> result;
    for (const auto* row : pg_attrdef_rows_) {
        if (row->adrelid == adrelid)
            result.push_back(row);
    }
    return result;
}

bool Catalog::DeleteAttrdef(Oid oid) {
    PreWrite();
    auto it = std::find_if(pg_attrdef_rows_.begin(), pg_attrdef_rows_.end(),
                           [oid](const FormData_pg_attrdef* r) { return r->oid == oid; });
    if (it == pg_attrdef_rows_.end())
        return false;
    pg_attrdef_rows_.erase(it);
    return true;
}

std::size_t Catalog::DeleteAttrdefsForRelid(Oid adrelid) {
    PreWrite();
    auto original_size = pg_attrdef_rows_.size();
    pg_attrdef_rows_.erase(
        std::remove_if(pg_attrdef_rows_.begin(), pg_attrdef_rows_.end(),
                       [adrelid](const FormData_pg_attrdef* r) { return r->adrelid == adrelid; }),
        pg_attrdef_rows_.end());
    return original_size - pg_attrdef_rows_.size();
}

// --- Catalog: pg_depend ---

void Catalog::InsertDepend(FormData_pg_depend* row) {
    PreWrite();
    if (row == nullptr) {
        ereport(error::LogLevel::kError, "Catalog::InsertDepend: row is null");
    }
    pg_depend_rows_.push_back(row);
}

std::vector<const FormData_pg_depend*> Catalog::GetDependsByRef(Oid refclassid,
                                                                Oid refobjid) const {
    std::vector<const FormData_pg_depend*> result;
    for (const auto* row : pg_depend_rows_) {
        if (row->refclassid == refclassid && row->refobjid == refobjid)
            result.push_back(row);
    }
    return result;
}

std::vector<const FormData_pg_depend*> Catalog::GetDependsByObj(Oid classid, Oid objid) const {
    std::vector<const FormData_pg_depend*> result;
    for (const auto* row : pg_depend_rows_) {
        if (row->classid == classid && row->objid == objid)
            result.push_back(row);
    }
    return result;
}

std::size_t Catalog::DeleteDependsByObj(Oid classid, Oid objid) {
    PreWrite();
    auto original_size = pg_depend_rows_.size();
    pg_depend_rows_.erase(std::remove_if(pg_depend_rows_.begin(), pg_depend_rows_.end(),
                                         [classid, objid](const FormData_pg_depend* r) {
                                             return r->classid == classid && r->objid == objid;
                                         }),
                          pg_depend_rows_.end());
    return original_size - pg_depend_rows_.size();
}

std::size_t Catalog::DeleteDependsByRef(Oid refclassid, Oid refobjid) {
    PreWrite();
    auto original_size = pg_depend_rows_.size();
    pg_depend_rows_.erase(std::remove_if(pg_depend_rows_.begin(), pg_depend_rows_.end(),
                                         [refclassid, refobjid](const FormData_pg_depend* r) {
                                             return r->refclassid == refclassid &&
                                                    r->refobjid == refobjid;
                                         }),
                          pg_depend_rows_.end());
    return original_size - pg_depend_rows_.size();
}

// --- Catalog: pg_statistic ---

void Catalog::InsertStatistic(FormData_pg_statistic* row) {
    PreWrite();
    if (row == nullptr) {
        ereport(error::LogLevel::kError, "Catalog::InsertStatistic: row is null");
    }
    pg_statistic_rows_.push_back(row);
}

const FormData_pg_statistic* Catalog::GetStatistic(Oid starelid, int16_t staattnum) const {
    for (const auto* row : pg_statistic_rows_) {
        if (row->starelid == starelid && row->staattnum == staattnum)
            return row;
    }
    return nullptr;
}

std::size_t Catalog::DeleteStatisticsForRelid(Oid starelid) {
    PreWrite();
    auto original_size = pg_statistic_rows_.size();
    pg_statistic_rows_.erase(std::remove_if(pg_statistic_rows_.begin(), pg_statistic_rows_.end(),
                                            [starelid](const FormData_pg_statistic* r) {
                                                return r->starelid == starelid;
                                            }),
                             pg_statistic_rows_.end());
    return original_size - pg_statistic_rows_.size();
}

// --- Catalog: pg_inherits ---

void Catalog::InsertInherits(FormData_pg_inherits* row) {
    PreWrite();
    if (row == nullptr) {
        ereport(error::LogLevel::kError, "Catalog::InsertInherits: row is null");
    }
    pg_inherits_rows_.push_back(row);
}

std::vector<const FormData_pg_inherits*> Catalog::GetInheritsByParent(Oid inhparent) const {
    std::vector<const FormData_pg_inherits*> result;
    for (const auto* row : pg_inherits_rows_) {
        if (row->inhparent == inhparent)
            result.push_back(row);
    }
    return result;
}

const FormData_pg_inherits* Catalog::GetInheritsByChild(Oid inhrelid) const {
    for (const auto* row : pg_inherits_rows_) {
        if (row->inhrelid == inhrelid)
            return row;
    }
    return nullptr;
}

bool Catalog::DeleteInherits(Oid inhrelid) {
    PreWrite();
    auto it =
        std::find_if(pg_inherits_rows_.begin(), pg_inherits_rows_.end(),
                     [inhrelid](const FormData_pg_inherits* r) { return r->inhrelid == inhrelid; });
    if (it == pg_inherits_rows_.end())
        return false;
    pg_inherits_rows_.erase(it);
    return true;
}

// --- Catalog: pg_am ---

Oid Catalog::InsertAm(FormData_pg_am* row) {
    PreWrite();
    if (row == nullptr) {
        ereport(error::LogLevel::kError, "Catalog::InsertAm: row is null");
    }
    if (row->oid == kInvalidOid) {
        row->oid = AllocateOid();
    }
    pg_am_rows_.push_back(row);
    return row->oid;
}

const FormData_pg_am* Catalog::GetAmByOid(Oid oid) const {
    for (const auto* row : pg_am_rows_) {
        if (row->oid == oid)
            return row;
    }
    return nullptr;
}

const FormData_pg_am* Catalog::GetAmByName(const std::string& name) const {
    for (const auto* row : pg_am_rows_) {
        if (row->amname == name)
            return row;
    }
    return nullptr;
}

// --- Catalog: pg_tablespace ---

Oid Catalog::InsertTablespace(FormData_pg_tablespace* row) {
    PreWrite();
    if (row == nullptr) {
        ereport(error::LogLevel::kError, "Catalog::InsertTablespace: row is null");
    }
    if (row->oid == kInvalidOid) {
        row->oid = AllocateOid();
    }
    pg_tablespace_rows_.push_back(row);
    return row->oid;
}

const FormData_pg_tablespace* Catalog::GetTablespaceByOid(Oid oid) const {
    for (const auto* row : pg_tablespace_rows_) {
        if (row->oid == oid)
            return row;
    }
    return nullptr;
}

const FormData_pg_tablespace* Catalog::GetTablespaceByName(const std::string& name) const {
    for (const auto* row : pg_tablespace_rows_) {
        if (row->spcname == name)
            return row;
    }
    return nullptr;
}

bool Catalog::DeleteTablespace(Oid oid) {
    PreWrite();
    auto it = std::find_if(pg_tablespace_rows_.begin(), pg_tablespace_rows_.end(),
                           [oid](const FormData_pg_tablespace* r) { return r->oid == oid; });
    if (it == pg_tablespace_rows_.end())
        return false;
    pg_tablespace_rows_.erase(it);
    return true;
}

// --- Catalog: pg_trigger ---

Oid Catalog::InsertTrigger(FormData_pg_trigger* row) {
    PreWrite();
    if (row == nullptr) {
        ereport(error::LogLevel::kError, "Catalog::InsertTrigger: row is null");
    }
    if (row->oid == kInvalidOid) {
        row->oid = AllocateOid();
    }
    pg_trigger_rows_.push_back(row);
    return row->oid;
}

const FormData_pg_trigger* Catalog::GetTriggerByOid(Oid oid) const {
    for (const auto* row : pg_trigger_rows_) {
        if (row->oid == oid)
            return row;
    }
    return nullptr;
}

std::vector<const FormData_pg_trigger*> Catalog::GetTriggersByRelid(Oid tgrelid) const {
    std::vector<const FormData_pg_trigger*> result;
    for (const auto* row : pg_trigger_rows_) {
        if (row->tgrelid == tgrelid)
            result.push_back(row);
    }
    return result;
}

bool Catalog::DeleteTrigger(Oid oid) {
    PreWrite();
    auto it = std::find_if(pg_trigger_rows_.begin(), pg_trigger_rows_.end(),
                           [oid](const FormData_pg_trigger* r) { return r->oid == oid; });
    if (it == pg_trigger_rows_.end())
        return false;
    pg_trigger_rows_.erase(it);
    return true;
}

std::size_t Catalog::DeleteTriggersForRelid(Oid tgrelid) {
    PreWrite();
    auto original_size = pg_trigger_rows_.size();
    pg_trigger_rows_.erase(
        std::remove_if(pg_trigger_rows_.begin(), pg_trigger_rows_.end(),
                       [tgrelid](const FormData_pg_trigger* r) { return r->tgrelid == tgrelid; }),
        pg_trigger_rows_.end());
    return original_size - pg_trigger_rows_.size();
}

// --- Catalog: pg_rewrite ---

Oid Catalog::InsertRewrite(FormData_pg_rewrite* row) {
    PreWrite();
    if (row == nullptr) {
        ereport(error::LogLevel::kError, "Catalog::InsertRewrite: row is null");
    }
    if (row->oid == kInvalidOid) {
        row->oid = AllocateOid();
    }
    pg_rewrite_rows_.push_back(row);
    return row->oid;
}

const FormData_pg_rewrite* Catalog::GetRewriteByOid(Oid oid) const {
    for (const auto* row : pg_rewrite_rows_) {
        if (row->oid == oid)
            return row;
    }
    return nullptr;
}

std::vector<const FormData_pg_rewrite*> Catalog::GetRewritesByRelid(Oid ev_class) const {
    std::vector<const FormData_pg_rewrite*> result;
    for (const auto* row : pg_rewrite_rows_) {
        if (row->ev_class == ev_class)
            result.push_back(row);
    }
    return result;
}

bool Catalog::DeleteRewrite(Oid oid) {
    PreWrite();
    auto it = std::find_if(pg_rewrite_rows_.begin(), pg_rewrite_rows_.end(),
                           [oid](const FormData_pg_rewrite* r) { return r->oid == oid; });
    if (it == pg_rewrite_rows_.end())
        return false;
    pg_rewrite_rows_.erase(it);
    return true;
}

std::size_t Catalog::DeleteRewritesForRelid(Oid ev_class) {
    PreWrite();
    auto original_size = pg_rewrite_rows_.size();
    pg_rewrite_rows_.erase(std::remove_if(pg_rewrite_rows_.begin(), pg_rewrite_rows_.end(),
                                          [ev_class](const FormData_pg_rewrite* r) {
                                              return r->ev_class == ev_class;
                                          }),
                           pg_rewrite_rows_.end());
    return original_size - pg_rewrite_rows_.size();
}

// --- P1-2: Transactional catalog snapshot ---
//
// pgcpp does not implement per-row MVCC on catalog tables. Instead, the
// transaction system takes a deep-copy snapshot of all user-created catalog
// rows (oid >= kFirstNormalObjectId) at transaction start. On ROLLBACK, the
// snapshot is restored (undoing all DDL changes). On COMMIT, the catalog is
// persisted to disk if any DDL marked it dirty.
//
// Snapshot row copies use plain `new`/`delete` (NOT palloc) so their lifetime
// is independent of any memory context — the snapshot must survive transaction
// commit/abort, which may delete memory contexts. Restored rows are re-allocated
// via makePallocNode so they rejoin the normal memory-context ownership model.
//
// Limitations (documented in catalog.hpp):
//   - SAVEPOINT-level DDL rollback is not supported (only top-level).
//   - Crash recovery uses on-commit Save() rather than WAL replay.

namespace {

using pgcpp::nodes::destroyPallocNode;
using pgcpp::nodes::makePallocNode;

// CopyUserRowsToSnapshot — deep-copy user rows (oid_field >= kFirstNormalObjectId)
// from a live vector into a snapshot vector using plain `new`.
template<typename Row>
void CopyUserRowsToSnapshot(const std::vector<Row*>& src, std::vector<Row*>& dst,
                            Oid Row::*oid_field) {
    for (const auto* r : src) {
        if ((r->*oid_field) >= kFirstNormalObjectId) {
            dst.push_back(new Row(*r));
        }
    }
}

// FreeUserRows — free all user rows (oid_field >= kFirstNormalObjectId) from a
// live vector via destroyPallocNode, then erase the nullified slots. Built-in
// rows (oid_field < kFirstNormalObjectId) are preserved.
template<typename Row>
void FreeUserRows(std::vector<Row*>& vec, Oid Row::*oid_field) {
    for (auto*& r : vec) {
        if ((r->*oid_field) >= kFirstNormalObjectId) {
            destroyPallocNode(r);
            r = nullptr;
        }
    }
    vec.erase(std::remove(vec.begin(), vec.end(), nullptr), vec.end());
}

// RestoreUserRows — deep-copy snapshot rows back into a live vector using
// makePallocNode so they are owned by the current memory context.
template<typename Row>
void RestoreUserRows(const std::vector<Row*>& src, std::vector<Row*>& dst) {
    for (const auto* r : src) {
        dst.push_back(makePallocNode<Row>(*r));
    }
}

// DeleteSnapshotRows — free all snapshot-owned rows (allocated with plain `new`).
template<typename Row>
void DeleteSnapshotRows(std::vector<Row*>& vec) {
    for (auto* r : vec) {
        delete r;
    }
    vec.clear();
}

}  // namespace

struct Catalog::CatalogSnapshot {
    std::vector<FormData_pg_class*> pg_class_rows;
    std::vector<FormData_pg_attribute*> pg_attribute_rows;
    std::vector<FormData_pg_type*> pg_type_rows;
    std::vector<FormData_pg_operator*> pg_operator_rows;
    std::vector<FormData_pg_proc*> pg_proc_rows;
    std::vector<FormData_pg_cast*> pg_cast_rows;
    std::vector<FormData_pg_aggregate*> pg_aggregate_rows;
    std::vector<FormData_pg_collation*> pg_collation_rows;
    std::vector<FormData_pg_namespace*> pg_namespace_rows;
    std::vector<FormData_pg_database*> pg_database_rows;
    std::vector<FormData_pg_index*> pg_index_rows;
    std::vector<FormData_pg_constraint*> pg_constraint_rows;
    std::vector<FormData_pg_attrdef*> pg_attrdef_rows;
    std::vector<FormData_pg_depend*> pg_depend_rows;
    std::vector<FormData_pg_statistic*> pg_statistic_rows;
    std::vector<FormData_pg_inherits*> pg_inherits_rows;
    std::vector<FormData_pg_am*> pg_am_rows;
    std::vector<FormData_pg_tablespace*> pg_tablespace_rows;
    std::vector<FormData_pg_trigger*> pg_trigger_rows;
    std::vector<FormData_pg_rewrite*> pg_rewrite_rows;
    Oid next_oid = kFirstNormalObjectId;

    ~CatalogSnapshot() {
        DeleteSnapshotRows(pg_class_rows);
        DeleteSnapshotRows(pg_attribute_rows);
        DeleteSnapshotRows(pg_type_rows);
        DeleteSnapshotRows(pg_operator_rows);
        DeleteSnapshotRows(pg_proc_rows);
        DeleteSnapshotRows(pg_cast_rows);
        DeleteSnapshotRows(pg_aggregate_rows);
        DeleteSnapshotRows(pg_collation_rows);
        DeleteSnapshotRows(pg_namespace_rows);
        DeleteSnapshotRows(pg_database_rows);
        DeleteSnapshotRows(pg_index_rows);
        DeleteSnapshotRows(pg_constraint_rows);
        DeleteSnapshotRows(pg_attrdef_rows);
        DeleteSnapshotRows(pg_depend_rows);
        DeleteSnapshotRows(pg_statistic_rows);
        DeleteSnapshotRows(pg_inherits_rows);
        DeleteSnapshotRows(pg_am_rows);
        DeleteSnapshotRows(pg_tablespace_rows);
        DeleteSnapshotRows(pg_trigger_rows);
        DeleteSnapshotRows(pg_rewrite_rows);
    }
};

void Catalog::TakeSnapshot() {
    DiscardSnapshot();  // discard any existing snapshot first
    snapshot_ = new CatalogSnapshot();
    CopyUserRowsToSnapshot(pg_class_rows_, snapshot_->pg_class_rows, &FormData_pg_class::oid);
    CopyUserRowsToSnapshot(pg_attribute_rows_, snapshot_->pg_attribute_rows,
                           &FormData_pg_attribute::attrelid);
    CopyUserRowsToSnapshot(pg_type_rows_, snapshot_->pg_type_rows, &FormData_pg_type::oid);
    CopyUserRowsToSnapshot(pg_operator_rows_, snapshot_->pg_operator_rows,
                           &FormData_pg_operator::oid);
    CopyUserRowsToSnapshot(pg_proc_rows_, snapshot_->pg_proc_rows, &FormData_pg_proc::oid);
    CopyUserRowsToSnapshot(pg_cast_rows_, snapshot_->pg_cast_rows, &FormData_pg_cast::oid);
    CopyUserRowsToSnapshot(pg_aggregate_rows_, snapshot_->pg_aggregate_rows,
                           &FormData_pg_aggregate::aggfnoid);
    CopyUserRowsToSnapshot(pg_collation_rows_, snapshot_->pg_collation_rows,
                           &FormData_pg_collation::oid);
    CopyUserRowsToSnapshot(pg_namespace_rows_, snapshot_->pg_namespace_rows,
                           &FormData_pg_namespace::oid);
    CopyUserRowsToSnapshot(pg_database_rows_, snapshot_->pg_database_rows,
                           &FormData_pg_database::oid);
    CopyUserRowsToSnapshot(pg_index_rows_, snapshot_->pg_index_rows,
                           &FormData_pg_index::indexrelid);
    CopyUserRowsToSnapshot(pg_constraint_rows_, snapshot_->pg_constraint_rows,
                           &FormData_pg_constraint::oid);
    CopyUserRowsToSnapshot(pg_attrdef_rows_, snapshot_->pg_attrdef_rows, &FormData_pg_attrdef::oid);
    CopyUserRowsToSnapshot(pg_depend_rows_, snapshot_->pg_depend_rows, &FormData_pg_depend::objid);
    CopyUserRowsToSnapshot(pg_statistic_rows_, snapshot_->pg_statistic_rows,
                           &FormData_pg_statistic::starelid);
    CopyUserRowsToSnapshot(pg_inherits_rows_, snapshot_->pg_inherits_rows,
                           &FormData_pg_inherits::inhrelid);
    CopyUserRowsToSnapshot(pg_am_rows_, snapshot_->pg_am_rows, &FormData_pg_am::oid);
    CopyUserRowsToSnapshot(pg_tablespace_rows_, snapshot_->pg_tablespace_rows,
                           &FormData_pg_tablespace::oid);
    CopyUserRowsToSnapshot(pg_trigger_rows_, snapshot_->pg_trigger_rows, &FormData_pg_trigger::oid);
    CopyUserRowsToSnapshot(pg_rewrite_rows_, snapshot_->pg_rewrite_rows, &FormData_pg_rewrite::oid);
    snapshot_->next_oid = next_oid_;
}

void Catalog::RestoreSnapshot() {
    if (snapshot_ == nullptr) {
        return;
    }
    // Free all current user rows (built-in rows are preserved).
    FreeUserRows(pg_class_rows_, &FormData_pg_class::oid);
    FreeUserRows(pg_attribute_rows_, &FormData_pg_attribute::attrelid);
    FreeUserRows(pg_type_rows_, &FormData_pg_type::oid);
    FreeUserRows(pg_operator_rows_, &FormData_pg_operator::oid);
    FreeUserRows(pg_proc_rows_, &FormData_pg_proc::oid);
    FreeUserRows(pg_cast_rows_, &FormData_pg_cast::oid);
    FreeUserRows(pg_aggregate_rows_, &FormData_pg_aggregate::aggfnoid);
    FreeUserRows(pg_collation_rows_, &FormData_pg_collation::oid);
    FreeUserRows(pg_namespace_rows_, &FormData_pg_namespace::oid);
    FreeUserRows(pg_database_rows_, &FormData_pg_database::oid);
    FreeUserRows(pg_index_rows_, &FormData_pg_index::indexrelid);
    FreeUserRows(pg_constraint_rows_, &FormData_pg_constraint::oid);
    FreeUserRows(pg_attrdef_rows_, &FormData_pg_attrdef::oid);
    FreeUserRows(pg_depend_rows_, &FormData_pg_depend::objid);
    FreeUserRows(pg_statistic_rows_, &FormData_pg_statistic::starelid);
    FreeUserRows(pg_inherits_rows_, &FormData_pg_inherits::inhrelid);
    FreeUserRows(pg_am_rows_, &FormData_pg_am::oid);
    FreeUserRows(pg_tablespace_rows_, &FormData_pg_tablespace::oid);
    FreeUserRows(pg_trigger_rows_, &FormData_pg_trigger::oid);
    FreeUserRows(pg_rewrite_rows_, &FormData_pg_rewrite::oid);
    // Deep-copy snapshot rows back into the live vectors.
    RestoreUserRows(snapshot_->pg_class_rows, pg_class_rows_);
    RestoreUserRows(snapshot_->pg_attribute_rows, pg_attribute_rows_);
    RestoreUserRows(snapshot_->pg_type_rows, pg_type_rows_);
    RestoreUserRows(snapshot_->pg_operator_rows, pg_operator_rows_);
    RestoreUserRows(snapshot_->pg_proc_rows, pg_proc_rows_);
    RestoreUserRows(snapshot_->pg_cast_rows, pg_cast_rows_);
    RestoreUserRows(snapshot_->pg_aggregate_rows, pg_aggregate_rows_);
    RestoreUserRows(snapshot_->pg_collation_rows, pg_collation_rows_);
    RestoreUserRows(snapshot_->pg_namespace_rows, pg_namespace_rows_);
    RestoreUserRows(snapshot_->pg_database_rows, pg_database_rows_);
    RestoreUserRows(snapshot_->pg_index_rows, pg_index_rows_);
    RestoreUserRows(snapshot_->pg_constraint_rows, pg_constraint_rows_);
    RestoreUserRows(snapshot_->pg_attrdef_rows, pg_attrdef_rows_);
    RestoreUserRows(snapshot_->pg_depend_rows, pg_depend_rows_);
    RestoreUserRows(snapshot_->pg_statistic_rows, pg_statistic_rows_);
    RestoreUserRows(snapshot_->pg_inherits_rows, pg_inherits_rows_);
    RestoreUserRows(snapshot_->pg_am_rows, pg_am_rows_);
    RestoreUserRows(snapshot_->pg_tablespace_rows, pg_tablespace_rows_);
    RestoreUserRows(snapshot_->pg_trigger_rows, pg_trigger_rows_);
    RestoreUserRows(snapshot_->pg_rewrite_rows, pg_rewrite_rows_);
    // Note: next_oid_ is intentionally NOT restored. Restoring it would cause
    // the next CREATE TABLE to reuse an OID whose physical storage file still
    // exists on disk (the MVP has no WAL redo / pendingDeletes mechanism to
    // clean up files from rolled-back DDL). An OID gap after ROLLBACK is
    // acceptable and consistent with the MVP's "no WAL" durability model.
    dirty_ = false;  // catalog now matches the pre-transaction state
}

void Catalog::DiscardSnapshot() {
    if (snapshot_ == nullptr) {
        return;
    }
    delete snapshot_;
    snapshot_ = nullptr;
}

void Catalog::CommitDirty() {
    if (dirty_ && !persist_path_.empty()) {
        Save(persist_path_);
        dirty_ = false;
    }
}

void Catalog::PreWrite() {
    // Only track dirty state when a transaction snapshot is active. Outside
    // a transaction (e.g., bootstrap), writes don't need rollback tracking.
    if (snapshot_ != nullptr) {
        dirty_ = true;
    }
}

// --- Persistence helpers (A-3) ---
//
// Catalog rows are serialized to a simple TSV file. Only user-created rows
// (oid >= kFirstNormalObjectId, or for key-less tables, rows whose owning
// OID is in the user range) are persisted, on top of BootstrapCatalog's
// built-in rows. The format is:
//
//   PGCPP_CATALOG_V1
//   next_oid\t<N>
//   [pg_class]
//   <field>\t<field>\t...      (one row per line)
//   [pg_attribute]
//   ...
//
// Strings escape '\\', '\t', '\n', '\r' so every physical line is exactly one
// row or one section header. Missing file is not an error (fresh initdb).
namespace {

using pgcpp::nodes::makePallocNode;

constexpr const char* kCatalogMagic = "PGCPP_CATALOG_V1";

// Escape a string field so it cannot contain a raw tab or newline.
std::string EscapeStr(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            default:
                out += c;
                break;
        }
    }
    return out;
}

std::string UnescapeStr(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            switch (s[i + 1]) {
                case '\\':
                    out += '\\';
                    ++i;
                    break;
                case 't':
                    out += '\t';
                    ++i;
                    break;
                case 'n':
                    out += '\n';
                    ++i;
                    break;
                case 'r':
                    out += '\r';
                    ++i;
                    break;
                default:
                    out += s[i];
                    break;  // keep unknown escape as-is
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

// Split a line on unescaped tabs; unescape each resulting field.
std::vector<std::string> SplitTab(const std::string& line) {
    std::vector<std::string> fields;
    std::string cur;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\\' && i + 1 < line.size()) {
            cur += line[i];
            cur += line[i + 1];
            ++i;
        } else if (line[i] == '\t') {
            fields.push_back(UnescapeStr(cur));
            cur.clear();
        } else {
            cur += line[i];
        }
    }
    fields.push_back(UnescapeStr(cur));
    return fields;
}

std::string JoinTab(const std::vector<std::string>& fields) {
    std::string out;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i > 0)
            out += '\t';
        out += fields[i];
    }
    return out;
}

// --- Numeric parsers (exception-free; AGENTS.md restricts exceptions to ereport) ---

bool ParseU32(const std::string& s, uint32_t& out) {
    errno = 0;
    char* end = nullptr;
    unsigned long v = std::strtoul(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str() || *end != '\0')
        return false;
    out = static_cast<uint32_t>(v);
    return true;
}

bool ParseI32(const std::string& s, int32_t& out) {
    errno = 0;
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str() || *end != '\0')
        return false;
    out = static_cast<int32_t>(v);
    return true;
}

bool ParseI16(const std::string& s, int16_t& out) {
    int32_t v = 0;
    if (!ParseI32(s, v))
        return false;
    out = static_cast<int16_t>(v);
    return true;
}

bool ParseFloat(const std::string& s, float& out) {
    errno = 0;
    char* end = nullptr;
    float v = std::strtof(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0')
        return false;
    out = v;
    return true;
}

// --- Field formatters (overload set) ---

std::string Fmt(bool b) {
    return b ? "1" : "0";
}
std::string Fmt(uint32_t v) {
    return std::to_string(v);
}
std::string Fmt(int32_t v) {
    return std::to_string(v);
}
std::string Fmt(int16_t v) {
    return std::to_string(v);
}
std::string Fmt(float v) {
    return std::to_string(v);
}
std::string Fmt(char c) {
    return std::string(1, c);
}
std::string Fmt(const std::string& v) {
    return EscapeStr(v);
}

template<typename E>
std::string FmtEnum(E e) {
    return std::string(1, static_cast<char>(e));
}

std::string FmtOidVec(const std::vector<Oid>& v) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i > 0)
            out += ',';
        out += std::to_string(v[i]);
    }
    return out;
}

std::string FmtStrVec(const std::vector<std::string>& v) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i > 0)
            out += ',';
        // Escape backslash and comma within elements.
        for (char c : v[i]) {
            if (c == '\\' || c == ',')
                out += '\\';
            out += c;
        }
    }
    return out;
}

std::vector<Oid> ParseOidVec(const std::string& s) {
    std::vector<Oid> out;
    std::string cur;
    auto flush = [&]() {
        if (!cur.empty()) {
            uint32_t v = 0;
            if (ParseU32(cur, v))
                out.push_back(v);
            cur.clear();
        }
    };
    for (char c : s) {
        if (c == ',')
            flush();
        else
            cur += c;
    }
    flush();
    return out;
}

std::vector<std::string> ParseStrVec(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            cur += s[i + 1];
            ++i;
        } else if (s[i] == ',') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += s[i];
        }
    }
    out.push_back(cur);
    return out;
}

// --- Per-struct serializers (field order matches struct declaration order) ---

std::vector<std::string> Ser(const FormData_pg_class& r) {
    return {Fmt(r.oid),
            Fmt(r.relname),
            Fmt(r.relnamespace),
            Fmt(r.reltype),
            Fmt(r.reloftype),
            Fmt(r.relowner),
            Fmt(r.relam),
            Fmt(r.relfilenode),
            Fmt(r.reltablespace),
            Fmt(r.relpages),
            Fmt(r.reltuples),
            Fmt(r.reltoastrelid),
            Fmt(r.relhasindex),
            Fmt(r.relisshared),
            FmtEnum(r.relpersistence),
            FmtEnum(r.relkind),
            Fmt(r.relnatts),
            Fmt(r.relchecks),
            Fmt(r.relhasrules),
            Fmt(r.relhastriggers),
            Fmt(r.relrowsecurity),
            Fmt(r.relforcerowsecurity),
            Fmt(r.relispopulated),
            Fmt(r.relreplident),
            Fmt(r.relispartition),
            Fmt(r.relfrozenxid),
            Fmt(r.relminmxid)};
}

std::vector<std::string> Ser(const FormData_pg_attribute& r) {
    return {Fmt(r.attrelid),     Fmt(r.attname),      Fmt(r.atttypid),       Fmt(r.attstattarget),
            Fmt(r.attlen),       Fmt(r.attnum),       Fmt(r.attndims),       Fmt(r.attcacheoff),
            Fmt(r.atttypmod),    Fmt(r.attbyval),     FmtEnum(r.attstorage), FmtEnum(r.attalign),
            Fmt(r.attnotnull),   Fmt(r.atthasdef),    Fmt(r.atthasmissing),  Fmt(r.attidentity),
            Fmt(r.attgenerated), Fmt(r.attisdropped), Fmt(r.attislocal),     Fmt(r.attinhcount),
            Fmt(r.attcollation)};
}

std::vector<std::string> Ser(const FormData_pg_type& r) {
    return {Fmt(r.oid),          Fmt(r.typname),         Fmt(r.typnamespace),
            Fmt(r.typowner),     Fmt(r.typlen),          Fmt(r.typbyval),
            FmtEnum(r.typtype),  FmtEnum(r.typcategory), Fmt(r.typispreferred),
            Fmt(r.typisdefined), Fmt(r.typdelim),        Fmt(r.typrelid),
            Fmt(r.typelem),      Fmt(r.typarray),        Fmt(r.typinput),
            Fmt(r.typoutput),    Fmt(r.typreceive),      Fmt(r.typsend),
            Fmt(r.typmodin),     Fmt(r.typmodout),       Fmt(r.typanalyze),
            FmtEnum(r.typalign), FmtEnum(r.typstorage),  Fmt(r.typnotnull),
            Fmt(r.typbasetype),  Fmt(r.typtypmod),       Fmt(r.typndims),
            Fmt(r.typcollation), Fmt(r.typdefault),      Fmt(r.typdefaultbin)};
}

std::vector<std::string> Ser(const FormData_pg_operator& r) {
    return {Fmt(r.oid),         Fmt(r.oprname),     Fmt(r.oprnamespace), Fmt(r.oprowner),
            FmtEnum(r.oprkind), Fmt(r.oprcanmerge), Fmt(r.oprcanhash),   Fmt(r.oprleft),
            Fmt(r.oprright),    Fmt(r.oprresult),   Fmt(r.oprcom),       Fmt(r.oprnegate),
            Fmt(r.oprcode),     Fmt(r.oprrest),     Fmt(r.oprjoin)};
}

std::vector<std::string> Ser(const FormData_pg_proc& r) {
    return {Fmt(r.oid),
            Fmt(r.proname),
            Fmt(r.pronamespace),
            Fmt(r.proowner),
            Fmt(r.prolang),
            Fmt(r.procost),
            Fmt(r.prorows),
            Fmt(r.provariadic),
            Fmt(r.prosupport),
            FmtEnum(r.prokind),
            Fmt(r.prosecdef),
            Fmt(r.proleakproof),
            Fmt(r.proisstrict),
            Fmt(r.proretset),
            FmtEnum(r.provolatile),
            FmtEnum(r.proparallel),
            Fmt(r.pronargs),
            Fmt(r.pronargdefaults),
            Fmt(r.prorettype),
            FmtOidVec(r.proargtypes),
            FmtOidVec(r.proallargtypes),
            Fmt(r.proargmodes),
            Fmt(r.proargnames),
            Fmt(r.proargdefaults),
            FmtOidVec(r.protrftypes),
            Fmt(r.prosrc),
            Fmt(r.probin),
            Fmt(r.prosqlbody),
            FmtStrVec(r.proconfig),
            FmtStrVec(r.proacl)};
}

std::vector<std::string> Ser(const FormData_pg_cast& r) {
    return {Fmt(r.oid),      Fmt(r.castsource),      Fmt(r.casttarget),
            Fmt(r.castfunc), FmtEnum(r.castcontext), FmtEnum(r.castmethod)};
}

std::vector<std::string> Ser(const FormData_pg_aggregate& r) {
    return {Fmt(r.aggfnoid),       FmtEnum(r.aggkind),        Fmt(r.aggnumdirectargs),
            Fmt(r.aggtransfn),     Fmt(r.aggfinalfn),         Fmt(r.aggcombinefn),
            Fmt(r.aggserialfn),    Fmt(r.aggdeserialfn),      Fmt(r.aggmtransfn),
            Fmt(r.aggminvtransfn), Fmt(r.aggmfinalfn),        Fmt(r.aggfinalextra),
            Fmt(r.aggmfinalextra), FmtEnum(r.aggfinalmodify), FmtEnum(r.aggmfinalmodify),
            Fmt(r.aggsortop),      Fmt(r.aggtranstype),       Fmt(r.aggtransspace),
            Fmt(r.aggmtranstype),  Fmt(r.aggmtransspace),     Fmt(r.agginitval),
            Fmt(r.aggminitval)};
}

std::vector<std::string> Ser(const FormData_pg_collation& r) {
    return {Fmt(r.oid),           Fmt(r.collname),         Fmt(r.collnamespace),
            Fmt(r.collowner),     FmtEnum(r.collprovider), Fmt(r.collisdeterministic),
            Fmt(r.collencoding),  Fmt(r.collcollate),      Fmt(r.collctype),
            Fmt(r.colliculocale), Fmt(r.collversion)};
}

// --- New table serializers (P0-6) ---

std::string FmtI16Vec(const std::vector<int16_t>& v) {
    return FmtOidVec(std::vector<Oid>(v.begin(), v.end()));
}

std::vector<std::string> Ser(const FormData_pg_namespace& r) {
    return {Fmt(r.oid), Fmt(r.nspname), Fmt(r.nspowner), Fmt(r.nspacl)};
}

std::vector<std::string> Ser(const FormData_pg_database& r) {
    return {Fmt(r.oid),          Fmt(r.datname),       Fmt(r.datdba),        Fmt(r.encoding),
            Fmt(r.datcollate),   Fmt(r.datctype),      Fmt(r.datistemplate), Fmt(r.datallowconn),
            Fmt(r.datconnlimit), Fmt(r.datlastsysoid), Fmt(r.datsize),       Fmt(r.datacl),
            Fmt(r.datfrozenxid), Fmt(r.datminmxid)};
}

std::vector<std::string> Ser(const FormData_pg_index& r) {
    return {Fmt(r.indexrelid),     Fmt(r.indrelid),
            Fmt(r.indnatts),       Fmt(r.indnkeyatts),
            Fmt(r.indisunique),    Fmt(r.indisprimary),
            Fmt(r.indisexclusion), Fmt(r.indisimmediate),
            Fmt(r.indisclustered), Fmt(r.indisvalid),
            Fmt(r.indcheckxmin),   Fmt(r.indisready),
            Fmt(r.indislive),      Fmt(r.indisreplident),
            FmtI16Vec(r.indkey),   FmtOidVec(r.indcollation),
            FmtOidVec(r.indclass), FmtI16Vec(r.indoption),
            Fmt(r.indpred),        Fmt(r.indexprs)};
}

std::vector<std::string> Ser(const FormData_pg_constraint& r) {
    return {Fmt(r.oid),
            Fmt(r.conname),
            Fmt(r.connamespace),
            FmtEnum(r.contype),
            Fmt(r.condeferrable),
            Fmt(r.condeferred),
            Fmt(r.convalidated),
            Fmt(r.conrelid),
            Fmt(r.contypid),
            Fmt(r.conindid),
            Fmt(r.conparentid),
            FmtI16Vec(r.conkey),
            FmtI16Vec(r.confkey),
            FmtOidVec(r.conpfeqop),
            FmtOidVec(r.conppeqop),
            FmtOidVec(r.conffeqop),
            FmtOidVec(r.confdelsetcols),
            FmtEnum(r.confupdtype),
            FmtEnum(r.confdeltype),
            FmtEnum(r.confmatchtype),
            Fmt(r.conislocal),
            Fmt(r.coninhcount),
            Fmt(r.connoinherit),
            Fmt(r.conbin),
            Fmt(r.consrc)};
}

std::vector<std::string> Ser(const FormData_pg_attrdef& r) {
    return {Fmt(r.oid), Fmt(r.adrelid), Fmt(r.adnum), Fmt(r.adbin), Fmt(r.adsrc)};
}

std::vector<std::string> Ser(const FormData_pg_depend& r) {
    return {Fmt(r.classid),  Fmt(r.objid),       Fmt(r.objsubid),   Fmt(r.refclassid),
            Fmt(r.refobjid), Fmt(r.refobjsubid), FmtEnum(r.deptype)};
}

std::vector<std::string> Ser(const FormData_pg_statistic& r) {
    return {Fmt(r.starelid),   Fmt(r.staattnum),   Fmt(r.stainherit), Fmt(r.stanullfrac),
            Fmt(r.stawidth),   Fmt(r.stadistinct), Fmt(r.stakind1),   Fmt(r.stakind2),
            Fmt(r.stakind3),   Fmt(r.stakind4),    Fmt(r.stakind5),   Fmt(r.staop1),
            Fmt(r.staop2),     Fmt(r.staop3),      Fmt(r.staop4),     Fmt(r.staop5),
            Fmt(r.stacoll1),   Fmt(r.stacoll2),    Fmt(r.stacoll3),   Fmt(r.stacoll4),
            Fmt(r.stacoll5),   Fmt(r.stavalues1),  Fmt(r.stavalues2), Fmt(r.stavalues3),
            Fmt(r.stavalues4), Fmt(r.stavalues5)};
}

std::vector<std::string> Ser(const FormData_pg_inherits& r) {
    return {Fmt(r.inhrelid), Fmt(r.inhparent), Fmt(r.inhseqnum)};
}

std::vector<std::string> Ser(const FormData_pg_am& r) {
    return {Fmt(r.oid),
            Fmt(r.amname),
            FmtEnum(r.amtype),
            Fmt(r.amhandler),
            Fmt(r.amcanorder),
            Fmt(r.amcanorderbyop),
            Fmt(r.amcanbackward),
            Fmt(r.amcanunique),
            Fmt(r.amcanmulticol),
            Fmt(r.amoptionalkey),
            Fmt(r.amsearcharray),
            Fmt(r.amsearchnulls),
            Fmt(r.amstorage),
            Fmt(r.amclusterable),
            Fmt(r.ampredlocks),
            Fmt(r.amkeytype),
            Fmt(r.amsummarizing),
            Fmt(r.amcaninclude),
            Fmt(r.amusemaintenanceworkmem)};
}

std::vector<std::string> Ser(const FormData_pg_tablespace& r) {
    return {Fmt(r.oid),        Fmt(r.spcname),     Fmt(r.spcowner),  Fmt(r.spcacl),
            Fmt(r.spcmaxsize), Fmt(r.spclocation), Fmt(r.spcoptions)};
}

std::vector<std::string> Ser(const FormData_pg_trigger& r) {
    return {Fmt(r.oid),          Fmt(r.tgrelid),       Fmt(r.tgparentid),     Fmt(r.tgname),
            Fmt(r.tgfoid),       FmtEnum(r.tgenabled), Fmt(r.tgisinternal),   Fmt(r.tgnargs),
            Fmt(r.tgargs),       FmtI16Vec(r.tgattr),  Fmt(r.tgconstrrelid),  Fmt(r.tgconstrindid),
            Fmt(r.tgconstraint), Fmt(r.tgdeferrable),  Fmt(r.tginitdeferred), Fmt(r.tgqual),
            Fmt(r.tgnewtable),   Fmt(r.tgoldtable)};
}

std::vector<std::string> Ser(const FormData_pg_rewrite& r) {
    return {Fmt(r.oid),        Fmt(r.ev_class),   Fmt(r.rulename), Fmt(r.ev_type),
            Fmt(r.ev_enabled), Fmt(r.is_instead), Fmt(r.ev_qual),  Fmt(r.ev_action)};
}

// --- Per-struct deserializers ---
//
// Each parses into a stack temporary; on any parse failure it returns
// nullptr (Load skips the row). On success it palloc-allocates a row via
// copy construction so std::string/std::vector members are registered for
// destruction with the owning MemoryContext.

FormData_pg_class* DeserPgClass(const std::vector<std::string>& f) {
    if (f.size() < 27)
        return nullptr;
    FormData_pg_class t;
    if (!ParseU32(f[0], t.oid))
        return nullptr;
    t.relname = f[1];
    if (!ParseU32(f[2], t.relnamespace))
        return nullptr;
    if (!ParseU32(f[3], t.reltype))
        return nullptr;
    if (!ParseU32(f[4], t.reloftype))
        return nullptr;
    if (!ParseU32(f[5], t.relowner))
        return nullptr;
    if (!ParseU32(f[6], t.relam))
        return nullptr;
    if (!ParseU32(f[7], t.relfilenode))
        return nullptr;
    if (!ParseU32(f[8], t.reltablespace))
        return nullptr;
    if (!ParseI32(f[9], t.relpages))
        return nullptr;
    if (!ParseFloat(f[10], t.reltuples))
        return nullptr;
    if (!ParseU32(f[11], t.reltoastrelid))
        return nullptr;
    if (f[12] != "0" && f[12] != "1")
        return nullptr;
    t.relhasindex = f[12] == "1";
    if (f[13] != "0" && f[13] != "1")
        return nullptr;
    t.relisshared = f[13] == "1";
    if (f[14].empty())
        return nullptr;
    t.relpersistence = static_cast<RelPersistence>(f[14][0]);
    if (f[15].empty())
        return nullptr;
    t.relkind = static_cast<RelKind>(f[15][0]);
    if (!ParseI16(f[16], t.relnatts))
        return nullptr;
    if (!ParseI16(f[17], t.relchecks))
        return nullptr;
    if (f[18] != "0" && f[18] != "1")
        return nullptr;
    t.relhasrules = f[18] == "1";
    if (f[19] != "0" && f[19] != "1")
        return nullptr;
    t.relhastriggers = f[19] == "1";
    if (f[20] != "0" && f[20] != "1")
        return nullptr;
    t.relrowsecurity = f[20] == "1";
    if (f[21] != "0" && f[21] != "1")
        return nullptr;
    t.relforcerowsecurity = f[21] == "1";
    if (f[22] != "0" && f[22] != "1")
        return nullptr;
    t.relispopulated = f[22] == "1";
    if (f[23].empty())
        return nullptr;
    t.relreplident = f[23][0];
    if (f[24] != "0" && f[24] != "1")
        return nullptr;
    t.relispartition = f[24] == "1";
    if (!ParseU32(f[25], t.relfrozenxid))
        return nullptr;
    if (!ParseU32(f[26], t.relminmxid))
        return nullptr;
    return makePallocNode<FormData_pg_class>(t);
}

FormData_pg_attribute* DeserPgAttribute(const std::vector<std::string>& f) {
    if (f.size() < 21)
        return nullptr;
    FormData_pg_attribute t;
    if (!ParseU32(f[0], t.attrelid))
        return nullptr;
    t.attname = f[1];
    if (!ParseU32(f[2], t.atttypid))
        return nullptr;
    if (!ParseI32(f[3], t.attstattarget))
        return nullptr;
    if (!ParseI16(f[4], t.attlen))
        return nullptr;
    if (!ParseI16(f[5], t.attnum))
        return nullptr;
    if (!ParseI16(f[6], t.attndims))
        return nullptr;
    if (!ParseI32(f[7], t.attcacheoff))
        return nullptr;
    if (!ParseI32(f[8], t.atttypmod))
        return nullptr;
    if (f[9] != "0" && f[9] != "1")
        return nullptr;
    t.attbyval = f[9] == "1";
    if (f[10].empty())
        return nullptr;
    t.attstorage = static_cast<AttStorage>(f[10][0]);
    if (f[11].empty())
        return nullptr;
    t.attalign = static_cast<AttAlign>(f[11][0]);
    if (f[12] != "0" && f[12] != "1")
        return nullptr;
    t.attnotnull = f[12] == "1";
    if (f[13] != "0" && f[13] != "1")
        return nullptr;
    t.atthasdef = f[13] == "1";
    if (f[14] != "0" && f[14] != "1")
        return nullptr;
    t.atthasmissing = f[14] == "1";
    t.attidentity = f[15].empty() ? '\0' : f[15][0];
    t.attgenerated = f[16].empty() ? '\0' : f[16][0];
    if (f[17] != "0" && f[17] != "1")
        return nullptr;
    t.attisdropped = f[17] == "1";
    if (f[18] != "0" && f[18] != "1")
        return nullptr;
    t.attislocal = f[18] == "1";
    if (!ParseI16(f[19], t.attinhcount))
        return nullptr;
    if (!ParseU32(f[20], t.attcollation))
        return nullptr;
    return makePallocNode<FormData_pg_attribute>(t);
}

FormData_pg_type* DeserPgType(const std::vector<std::string>& f) {
    if (f.size() < 30)
        return nullptr;
    FormData_pg_type t;
    if (!ParseU32(f[0], t.oid))
        return nullptr;
    t.typname = f[1];
    if (!ParseU32(f[2], t.typnamespace))
        return nullptr;
    if (!ParseU32(f[3], t.typowner))
        return nullptr;
    if (!ParseI16(f[4], t.typlen))
        return nullptr;
    if (f[5] != "0" && f[5] != "1")
        return nullptr;
    t.typbyval = f[5] == "1";
    if (f[6].empty())
        return nullptr;
    t.typtype = static_cast<TypeType>(f[6][0]);
    if (f[7].empty())
        return nullptr;
    t.typcategory = static_cast<TypeCategory>(f[7][0]);
    if (f[8] != "0" && f[8] != "1")
        return nullptr;
    t.typispreferred = f[8] == "1";
    if (f[9] != "0" && f[9] != "1")
        return nullptr;
    t.typisdefined = f[9] == "1";
    t.typdelim = f[10].empty() ? ',' : f[10][0];
    if (!ParseU32(f[11], t.typrelid))
        return nullptr;
    if (!ParseU32(f[12], t.typelem))
        return nullptr;
    if (!ParseU32(f[13], t.typarray))
        return nullptr;
    if (!ParseU32(f[14], t.typinput))
        return nullptr;
    if (!ParseU32(f[15], t.typoutput))
        return nullptr;
    if (!ParseU32(f[16], t.typreceive))
        return nullptr;
    if (!ParseU32(f[17], t.typsend))
        return nullptr;
    if (!ParseU32(f[18], t.typmodin))
        return nullptr;
    if (!ParseU32(f[19], t.typmodout))
        return nullptr;
    if (!ParseU32(f[20], t.typanalyze))
        return nullptr;
    if (f[21].empty())
        return nullptr;
    t.typalign = static_cast<TypeAlign>(f[21][0]);
    if (f[22].empty())
        return nullptr;
    t.typstorage = static_cast<TypeStorage>(f[22][0]);
    if (f[23] != "0" && f[23] != "1")
        return nullptr;
    t.typnotnull = f[23] == "1";
    if (!ParseU32(f[24], t.typbasetype))
        return nullptr;
    if (!ParseI32(f[25], t.typtypmod))
        return nullptr;
    if (!ParseI32(f[26], t.typndims))
        return nullptr;
    if (!ParseU32(f[27], t.typcollation))
        return nullptr;
    t.typdefault = f[28];
    t.typdefaultbin = f[29];
    return makePallocNode<FormData_pg_type>(t);
}

FormData_pg_operator* DeserPgOperator(const std::vector<std::string>& f) {
    if (f.size() < 15)
        return nullptr;
    FormData_pg_operator t;
    if (!ParseU32(f[0], t.oid))
        return nullptr;
    t.oprname = f[1];
    if (!ParseU32(f[2], t.oprnamespace))
        return nullptr;
    if (!ParseU32(f[3], t.oprowner))
        return nullptr;
    if (f[4].empty())
        return nullptr;
    t.oprkind = static_cast<OperatorKind>(f[4][0]);
    if (f[5] != "0" && f[5] != "1")
        return nullptr;
    t.oprcanmerge = f[5] == "1";
    if (f[6] != "0" && f[6] != "1")
        return nullptr;
    t.oprcanhash = f[6] == "1";
    if (!ParseU32(f[7], t.oprleft))
        return nullptr;
    if (!ParseU32(f[8], t.oprright))
        return nullptr;
    if (!ParseU32(f[9], t.oprresult))
        return nullptr;
    if (!ParseU32(f[10], t.oprcom))
        return nullptr;
    if (!ParseU32(f[11], t.oprnegate))
        return nullptr;
    if (!ParseU32(f[12], t.oprcode))
        return nullptr;
    if (!ParseU32(f[13], t.oprrest))
        return nullptr;
    if (!ParseU32(f[14], t.oprjoin))
        return nullptr;
    return makePallocNode<FormData_pg_operator>(t);
}

FormData_pg_proc* DeserPgProc(const std::vector<std::string>& f) {
    if (f.size() < 30)
        return nullptr;
    FormData_pg_proc t;
    if (!ParseU32(f[0], t.oid))
        return nullptr;
    t.proname = f[1];
    if (!ParseU32(f[2], t.pronamespace))
        return nullptr;
    if (!ParseU32(f[3], t.proowner))
        return nullptr;
    if (!ParseU32(f[4], t.prolang))
        return nullptr;
    if (!ParseFloat(f[5], t.procost))
        return nullptr;
    if (!ParseFloat(f[6], t.prorows))
        return nullptr;
    if (!ParseU32(f[7], t.provariadic))
        return nullptr;
    if (!ParseU32(f[8], t.prosupport))
        return nullptr;
    if (f[9].empty())
        return nullptr;
    t.prokind = static_cast<ProKind>(f[9][0]);
    if (f[10] != "0" && f[10] != "1")
        return nullptr;
    t.prosecdef = f[10] == "1";
    if (f[11] != "0" && f[11] != "1")
        return nullptr;
    t.proleakproof = f[11] == "1";
    if (f[12] != "0" && f[12] != "1")
        return nullptr;
    t.proisstrict = f[12] == "1";
    if (f[13] != "0" && f[13] != "1")
        return nullptr;
    t.proretset = f[13] == "1";
    if (f[14].empty())
        return nullptr;
    t.provolatile = static_cast<ProVolatile>(f[14][0]);
    if (f[15].empty())
        return nullptr;
    t.proparallel = static_cast<ProParallel>(f[15][0]);
    if (!ParseI16(f[16], t.pronargs))
        return nullptr;
    if (!ParseI16(f[17], t.pronargdefaults))
        return nullptr;
    if (!ParseU32(f[18], t.prorettype))
        return nullptr;
    t.proargtypes = ParseOidVec(f[19]);
    t.proallargtypes = ParseOidVec(f[20]);
    t.proargmodes = f[21];
    t.proargnames = f[22];
    t.proargdefaults = f[23];
    t.protrftypes = ParseOidVec(f[24]);
    t.prosrc = f[25];
    t.probin = f[26];
    t.prosqlbody = f[27];
    t.proconfig = ParseStrVec(f[28]);
    t.proacl = ParseStrVec(f[29]);
    return makePallocNode<FormData_pg_proc>(t);
}

FormData_pg_cast* DeserPgCast(const std::vector<std::string>& f) {
    if (f.size() < 6)
        return nullptr;
    FormData_pg_cast t;
    if (!ParseU32(f[0], t.oid))
        return nullptr;
    if (!ParseU32(f[1], t.castsource))
        return nullptr;
    if (!ParseU32(f[2], t.casttarget))
        return nullptr;
    if (!ParseU32(f[3], t.castfunc))
        return nullptr;
    if (f[4].empty())
        return nullptr;
    t.castcontext = static_cast<CastContext>(f[4][0]);
    if (f[5].empty())
        return nullptr;
    t.castmethod = static_cast<CastMethod>(f[5][0]);
    return makePallocNode<FormData_pg_cast>(t);
}

FormData_pg_aggregate* DeserPgAggregate(const std::vector<std::string>& f) {
    if (f.size() < 22)
        return nullptr;
    FormData_pg_aggregate t;
    if (!ParseU32(f[0], t.aggfnoid))
        return nullptr;
    if (f[1].empty())
        return nullptr;
    t.aggkind = static_cast<AggKind>(f[1][0]);
    if (!ParseI16(f[2], t.aggnumdirectargs))
        return nullptr;
    if (!ParseU32(f[3], t.aggtransfn))
        return nullptr;
    if (!ParseU32(f[4], t.aggfinalfn))
        return nullptr;
    if (!ParseU32(f[5], t.aggcombinefn))
        return nullptr;
    if (!ParseU32(f[6], t.aggserialfn))
        return nullptr;
    if (!ParseU32(f[7], t.aggdeserialfn))
        return nullptr;
    if (!ParseU32(f[8], t.aggmtransfn))
        return nullptr;
    if (!ParseU32(f[9], t.aggminvtransfn))
        return nullptr;
    if (!ParseU32(f[10], t.aggmfinalfn))
        return nullptr;
    if (f[11] != "0" && f[11] != "1")
        return nullptr;
    t.aggfinalextra = f[11] == "1";
    if (f[12] != "0" && f[12] != "1")
        return nullptr;
    t.aggmfinalextra = f[12] == "1";
    if (f[13].empty())
        return nullptr;
    t.aggfinalmodify = static_cast<AggModify>(f[13][0]);
    if (f[14].empty())
        return nullptr;
    t.aggmfinalmodify = static_cast<AggModify>(f[14][0]);
    if (!ParseU32(f[15], t.aggsortop))
        return nullptr;
    if (!ParseU32(f[16], t.aggtranstype))
        return nullptr;
    if (!ParseI32(f[17], t.aggtransspace))
        return nullptr;
    if (!ParseU32(f[18], t.aggmtranstype))
        return nullptr;
    if (!ParseI32(f[19], t.aggmtransspace))
        return nullptr;
    t.agginitval = f[20];
    t.aggminitval = f[21];
    return makePallocNode<FormData_pg_aggregate>(t);
}

FormData_pg_collation* DeserPgCollation(const std::vector<std::string>& f) {
    if (f.size() < 11)
        return nullptr;
    FormData_pg_collation t;
    if (!ParseU32(f[0], t.oid))
        return nullptr;
    t.collname = f[1];
    if (!ParseU32(f[2], t.collnamespace))
        return nullptr;
    if (!ParseU32(f[3], t.collowner))
        return nullptr;
    if (f[4].empty())
        return nullptr;
    t.collprovider = static_cast<CollProvider>(f[4][0]);
    if (f[5] != "0" && f[5] != "1")
        return nullptr;
    t.collisdeterministic = f[5] == "1";
    if (!ParseI32(f[6], t.collencoding))
        return nullptr;
    t.collcollate = f[7];
    t.collctype = f[8];
    t.colliculocale = f[9];
    t.collversion = f[10];
    return makePallocNode<FormData_pg_collation>(t);
}

// --- New table deserializers (P0-6) ---

std::vector<int16_t> ParseI16Vec(const std::string& s) {
    auto oids = ParseOidVec(s);
    return std::vector<int16_t>(oids.begin(), oids.end());
}

FormData_pg_namespace* DeserPgNamespace(const std::vector<std::string>& f) {
    if (f.size() < 4)
        return nullptr;
    FormData_pg_namespace t;
    if (!ParseU32(f[0], t.oid))
        return nullptr;
    t.nspname = f[1];
    if (!ParseU32(f[2], t.nspowner))
        return nullptr;
    if (f[3] != "0" && f[3] != "1")
        return nullptr;
    t.nspacl = f[3] == "1";
    return makePallocNode<FormData_pg_namespace>(t);
}

FormData_pg_database* DeserPgDatabase(const std::vector<std::string>& f) {
    if (f.size() < 14)
        return nullptr;
    FormData_pg_database t;
    if (!ParseU32(f[0], t.oid))
        return nullptr;
    t.datname = f[1];
    if (!ParseU32(f[2], t.datdba))
        return nullptr;
    if (!ParseI32(f[3], t.encoding))
        return nullptr;
    t.datcollate = f[4];
    t.datctype = f[5];
    if (f[6] != "0" && f[6] != "1")
        return nullptr;
    t.datistemplate = f[6] == "1";
    if (f[7] != "0" && f[7] != "1")
        return nullptr;
    t.datallowconn = f[7] == "1";
    if (!ParseI32(f[8], t.datconnlimit))
        return nullptr;
    if (!ParseU32(f[9], t.datlastsysoid))
        return nullptr;
    if (!ParseI32(f[10], t.datsize))
        return nullptr;
    if (f[11] != "0" && f[11] != "1")
        return nullptr;
    t.datacl = f[11] == "1";
    if (!ParseU32(f[12], t.datfrozenxid))
        return nullptr;
    if (!ParseU32(f[13], t.datminmxid))
        return nullptr;
    return makePallocNode<FormData_pg_database>(t);
}

FormData_pg_index* DeserPgIndex(const std::vector<std::string>& f) {
    if (f.size() < 20)
        return nullptr;
    FormData_pg_index t;
    if (!ParseU32(f[0], t.indexrelid))
        return nullptr;
    if (!ParseU32(f[1], t.indrelid))
        return nullptr;
    if (!ParseI16(f[2], t.indnatts))
        return nullptr;
    if (!ParseI16(f[3], t.indnkeyatts))
        return nullptr;
    if (f[4] != "0" && f[4] != "1")
        return nullptr;
    t.indisunique = f[4] == "1";
    if (f[5] != "0" && f[5] != "1")
        return nullptr;
    t.indisprimary = f[5] == "1";
    if (f[6] != "0" && f[6] != "1")
        return nullptr;
    t.indisexclusion = f[6] == "1";
    if (f[7] != "0" && f[7] != "1")
        return nullptr;
    t.indisimmediate = f[7] == "1";
    if (f[8] != "0" && f[8] != "1")
        return nullptr;
    t.indisclustered = f[8] == "1";
    if (f[9] != "0" && f[9] != "1")
        return nullptr;
    t.indisvalid = f[9] == "1";
    if (f[10] != "0" && f[10] != "1")
        return nullptr;
    t.indcheckxmin = f[10] == "1";
    if (f[11] != "0" && f[11] != "1")
        return nullptr;
    t.indisready = f[11] == "1";
    if (f[12] != "0" && f[12] != "1")
        return nullptr;
    t.indislive = f[12] == "1";
    if (f[13] != "0" && f[13] != "1")
        return nullptr;
    t.indisreplident = f[13] == "1";
    t.indkey = ParseI16Vec(f[14]);
    t.indcollation = ParseOidVec(f[15]);
    t.indclass = ParseOidVec(f[16]);
    t.indoption = ParseI16Vec(f[17]);
    if (!ParseU32(f[18], t.indpred))
        return nullptr;
    if (!ParseU32(f[19], t.indexprs))
        return nullptr;
    return makePallocNode<FormData_pg_index>(t);
}

FormData_pg_constraint* DeserPgConstraint(const std::vector<std::string>& f) {
    if (f.size() < 25)
        return nullptr;
    FormData_pg_constraint t;
    if (!ParseU32(f[0], t.oid))
        return nullptr;
    t.conname = f[1];
    if (!ParseU32(f[2], t.connamespace))
        return nullptr;
    if (f[3].empty())
        return nullptr;
    t.contype = static_cast<ConstraintType>(f[3][0]);
    if (f[4] != "0" && f[4] != "1")
        return nullptr;
    t.condeferrable = f[4] == "1";
    if (f[5] != "0" && f[5] != "1")
        return nullptr;
    t.condeferred = f[5] == "1";
    if (f[6] != "0" && f[6] != "1")
        return nullptr;
    t.convalidated = f[6] == "1";
    if (!ParseU32(f[7], t.conrelid))
        return nullptr;
    if (!ParseU32(f[8], t.contypid))
        return nullptr;
    if (!ParseU32(f[9], t.conindid))
        return nullptr;
    if (!ParseU32(f[10], t.conparentid))
        return nullptr;
    t.conkey = ParseI16Vec(f[11]);
    t.confkey = ParseI16Vec(f[12]);
    t.conpfeqop = ParseOidVec(f[13]);
    t.conppeqop = ParseOidVec(f[14]);
    t.conffeqop = ParseOidVec(f[15]);
    t.confdelsetcols = ParseOidVec(f[16]);
    if (f[17].empty())
        return nullptr;
    t.confupdtype = static_cast<ConstraintAction>(f[17][0]);
    if (f[18].empty())
        return nullptr;
    t.confdeltype = static_cast<ConstraintAction>(f[18][0]);
    if (f[19].empty())
        return nullptr;
    t.confmatchtype = static_cast<ConstraintMatch>(f[19][0]);
    if (f[20] != "0" && f[20] != "1")
        return nullptr;
    t.conislocal = f[20] == "1";
    if (!ParseI16(f[21], t.coninhcount))
        return nullptr;
    if (f[22] != "0" && f[22] != "1")
        return nullptr;
    t.connoinherit = f[22] == "1";
    t.conbin = f[23];
    t.consrc = f[24];
    return makePallocNode<FormData_pg_constraint>(t);
}

FormData_pg_attrdef* DeserPgAttrdef(const std::vector<std::string>& f) {
    if (f.size() < 5)
        return nullptr;
    FormData_pg_attrdef t;
    if (!ParseU32(f[0], t.oid))
        return nullptr;
    if (!ParseU32(f[1], t.adrelid))
        return nullptr;
    if (!ParseI16(f[2], t.adnum))
        return nullptr;
    t.adbin = f[3];
    t.adsrc = f[4];
    return makePallocNode<FormData_pg_attrdef>(t);
}

FormData_pg_depend* DeserPgDepend(const std::vector<std::string>& f) {
    if (f.size() < 7)
        return nullptr;
    FormData_pg_depend t;
    if (!ParseU32(f[0], t.classid))
        return nullptr;
    if (!ParseU32(f[1], t.objid))
        return nullptr;
    if (!ParseI32(f[2], t.objsubid))
        return nullptr;
    if (!ParseU32(f[3], t.refclassid))
        return nullptr;
    if (!ParseU32(f[4], t.refobjid))
        return nullptr;
    if (!ParseI32(f[5], t.refobjsubid))
        return nullptr;
    if (f[6].empty())
        return nullptr;
    t.deptype = static_cast<DependencyType>(f[6][0]);
    return makePallocNode<FormData_pg_depend>(t);
}

FormData_pg_statistic* DeserPgStatistic(const std::vector<std::string>& f) {
    if (f.size() < 26)
        return nullptr;
    FormData_pg_statistic t;
    if (!ParseU32(f[0], t.starelid))
        return nullptr;
    if (!ParseI16(f[1], t.staattnum))
        return nullptr;
    if (f[2] != "0" && f[2] != "1")
        return nullptr;
    t.stainherit = f[2] == "1";
    if (!ParseFloat(f[3], t.stanullfrac))
        return nullptr;
    if (!ParseI32(f[4], t.stawidth))
        return nullptr;
    if (!ParseI32(f[5], t.stadistinct))
        return nullptr;
    if (!ParseI16(f[6], t.stakind1))
        return nullptr;
    if (!ParseI16(f[7], t.stakind2))
        return nullptr;
    if (!ParseI16(f[8], t.stakind3))
        return nullptr;
    if (!ParseI16(f[9], t.stakind4))
        return nullptr;
    if (!ParseI16(f[10], t.stakind5))
        return nullptr;
    if (!ParseU32(f[11], t.staop1))
        return nullptr;
    if (!ParseU32(f[12], t.staop2))
        return nullptr;
    if (!ParseU32(f[13], t.staop3))
        return nullptr;
    if (!ParseU32(f[14], t.staop4))
        return nullptr;
    if (!ParseU32(f[15], t.staop5))
        return nullptr;
    if (!ParseU32(f[16], t.stacoll1))
        return nullptr;
    if (!ParseU32(f[17], t.stacoll2))
        return nullptr;
    if (!ParseU32(f[18], t.stacoll3))
        return nullptr;
    if (!ParseU32(f[19], t.stacoll4))
        return nullptr;
    if (!ParseU32(f[20], t.stacoll5))
        return nullptr;
    t.stavalues1 = f[21];
    t.stavalues2 = f[22];
    t.stavalues3 = f[23];
    t.stavalues4 = f[24];
    t.stavalues5 = f[25];
    return makePallocNode<FormData_pg_statistic>(t);
}

FormData_pg_inherits* DeserPgInherits(const std::vector<std::string>& f) {
    if (f.size() < 3)
        return nullptr;
    FormData_pg_inherits t;
    if (!ParseU32(f[0], t.inhrelid))
        return nullptr;
    if (!ParseU32(f[1], t.inhparent))
        return nullptr;
    if (!ParseI16(f[2], t.inhseqnum))
        return nullptr;
    return makePallocNode<FormData_pg_inherits>(t);
}

FormData_pg_am* DeserPgAm(const std::vector<std::string>& f) {
    if (f.size() < 18)
        return nullptr;
    FormData_pg_am t;
    if (!ParseU32(f[0], t.oid))
        return nullptr;
    t.amname = f[1];
    if (f[2].empty())
        return nullptr;
    t.amtype = static_cast<AmType>(f[2][0]);
    if (!ParseU32(f[3], t.amhandler))
        return nullptr;
    auto parse_bool = [](const std::string& s, bool& out) {
        if (s != "0" && s != "1")
            return false;
        out = s == "1";
        return true;
    };
    if (!parse_bool(f[4], t.amcanorder))
        return nullptr;
    if (!parse_bool(f[5], t.amcanorderbyop))
        return nullptr;
    if (!parse_bool(f[6], t.amcanbackward))
        return nullptr;
    if (!parse_bool(f[7], t.amcanunique))
        return nullptr;
    if (!parse_bool(f[8], t.amcanmulticol))
        return nullptr;
    if (!parse_bool(f[9], t.amoptionalkey))
        return nullptr;
    if (!parse_bool(f[10], t.amsearcharray))
        return nullptr;
    if (!parse_bool(f[11], t.amsearchnulls))
        return nullptr;
    if (!parse_bool(f[12], t.amstorage))
        return nullptr;
    if (!parse_bool(f[13], t.amclusterable))
        return nullptr;
    if (!parse_bool(f[14], t.ampredlocks))
        return nullptr;
    if (!parse_bool(f[15], t.amkeytype))
        return nullptr;
    if (!parse_bool(f[16], t.amsummarizing))
        return nullptr;
    if (!parse_bool(f[17], t.amcaninclude))
        return nullptr;
    // amusemaintenanceworkmem is field 18; allow older files without it.
    if (f.size() > 18) {
        if (!parse_bool(f[18], t.amusemaintenanceworkmem))
            return nullptr;
    }
    return makePallocNode<FormData_pg_am>(t);
}

FormData_pg_tablespace* DeserPgTablespace(const std::vector<std::string>& f) {
    if (f.size() < 7)
        return nullptr;
    FormData_pg_tablespace t;
    if (!ParseU32(f[0], t.oid))
        return nullptr;
    t.spcname = f[1];
    if (!ParseU32(f[2], t.spcowner))
        return nullptr;
    if (f[3] != "0" && f[3] != "1")
        return nullptr;
    t.spcacl = f[3] == "1";
    if (!ParseI32(f[4], t.spcmaxsize))
        return nullptr;
    t.spclocation = f[5];
    t.spcoptions = f[6];
    return makePallocNode<FormData_pg_tablespace>(t);
}

FormData_pg_trigger* DeserPgTrigger(const std::vector<std::string>& f) {
    if (f.size() < 18)
        return nullptr;
    FormData_pg_trigger t;
    if (!ParseU32(f[0], t.oid))
        return nullptr;
    if (!ParseU32(f[1], t.tgrelid))
        return nullptr;
    if (!ParseU32(f[2], t.tgparentid))
        return nullptr;
    t.tgname = f[3];
    if (!ParseU32(f[4], t.tgfoid))
        return nullptr;
    if (f[5].empty())
        return nullptr;
    t.tgenabled = static_cast<TriggerEnabled>(f[5][0]);
    if (f[6] != "0" && f[6] != "1")
        return nullptr;
    t.tgisinternal = f[6] == "1";
    if (!ParseI16(f[7], t.tgnargs))
        return nullptr;
    t.tgargs = f[8];
    t.tgattr = ParseI16Vec(f[9]);
    if (!ParseU32(f[10], t.tgconstrrelid))
        return nullptr;
    if (!ParseU32(f[11], t.tgconstrindid))
        return nullptr;
    if (!ParseU32(f[12], t.tgconstraint))
        return nullptr;
    if (f[13] != "0" && f[13] != "1")
        return nullptr;
    t.tgdeferrable = f[13] == "1";
    if (f[14] != "0" && f[14] != "1")
        return nullptr;
    t.tginitdeferred = f[14] == "1";
    if (!ParseU32(f[15], t.tgqual))
        return nullptr;
    t.tgnewtable = f[16];
    t.tgoldtable = f[17];
    return makePallocNode<FormData_pg_trigger>(t);
}

FormData_pg_rewrite* DeserPgRewrite(const std::vector<std::string>& f) {
    if (f.size() < 8)
        return nullptr;
    FormData_pg_rewrite t;
    if (!ParseU32(f[0], t.oid))
        return nullptr;
    if (!ParseU32(f[1], t.ev_class))
        return nullptr;
    t.rulename = f[2];
    if (f[3].empty())
        return nullptr;
    t.ev_type = f[3][0];
    if (f[4] != "0" && f[4] != "1")
        return nullptr;
    t.ev_enabled = f[4] == "1";
    if (f[5] != "0" && f[5] != "1")
        return nullptr;
    t.is_instead = f[5] == "1";
    if (!ParseU32(f[6], t.ev_qual))
        return nullptr;
    if (!ParseU32(f[7], t.ev_action))
        return nullptr;
    return makePallocNode<FormData_pg_rewrite>(t);
}

}  // namespace

// --- Catalog: persistence (A-3) ---

bool Catalog::Save(const std::string& path) const {
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out.is_open())
        return false;

    out << kCatalogMagic << '\n';
    out << "next_oid\t" << next_oid_ << '\n';

    auto write_section = [&](const char* name, const std::string& body) {
        out << name << '\n' << body;
    };

    auto emit_class = [&]() {
        std::string body;
        for (const auto* r : pg_class_rows_) {
            if (r->oid < kFirstNormalObjectId)
                continue;
            body += JoinTab(Ser(*r)) + '\n';
        }
        return body;
    };
    auto emit_attr = [&]() {
        std::string body;
        for (const auto* r : pg_attribute_rows_) {
            if (r->attrelid < kFirstNormalObjectId)
                continue;
            body += JoinTab(Ser(*r)) + '\n';
        }
        return body;
    };
    auto emit_type = [&]() {
        std::string body;
        for (const auto* r : pg_type_rows_) {
            if (r->oid < kFirstNormalObjectId)
                continue;
            body += JoinTab(Ser(*r)) + '\n';
        }
        return body;
    };
    auto emit_op = [&]() {
        std::string body;
        for (const auto* r : pg_operator_rows_) {
            if (r->oid < kFirstNormalObjectId)
                continue;
            body += JoinTab(Ser(*r)) + '\n';
        }
        return body;
    };
    auto emit_proc = [&]() {
        std::string body;
        for (const auto* r : pg_proc_rows_) {
            if (r->oid < kFirstNormalObjectId)
                continue;
            body += JoinTab(Ser(*r)) + '\n';
        }
        return body;
    };
    auto emit_cast = [&]() {
        std::string body;
        for (const auto* r : pg_cast_rows_) {
            if (r->oid < kFirstNormalObjectId)
                continue;
            body += JoinTab(Ser(*r)) + '\n';
        }
        return body;
    };
    auto emit_agg = [&]() {
        std::string body;
        for (const auto* r : pg_aggregate_rows_) {
            if (r->aggfnoid < kFirstNormalObjectId)
                continue;
            body += JoinTab(Ser(*r)) + '\n';
        }
        return body;
    };
    auto emit_coll = [&]() {
        std::string body;
        for (const auto* r : pg_collation_rows_) {
            if (r->oid < kFirstNormalObjectId)
                continue;
            body += JoinTab(Ser(*r)) + '\n';
        }
        return body;
    };
    auto emit_namespace = [&]() {
        std::string body;
        for (const auto* r : pg_namespace_rows_) {
            if (r->oid < kFirstNormalObjectId)
                continue;
            body += JoinTab(Ser(*r)) + '\n';
        }
        return body;
    };
    auto emit_database = [&]() {
        std::string body;
        for (const auto* r : pg_database_rows_) {
            if (r->oid < kFirstNormalObjectId)
                continue;
            body += JoinTab(Ser(*r)) + '\n';
        }
        return body;
    };
    auto emit_index = [&]() {
        std::string body;
        for (const auto* r : pg_index_rows_) {
            if (r->indexrelid < kFirstNormalObjectId)
                continue;
            body += JoinTab(Ser(*r)) + '\n';
        }
        return body;
    };
    auto emit_constraint = [&]() {
        std::string body;
        for (const auto* r : pg_constraint_rows_) {
            if (r->oid < kFirstNormalObjectId)
                continue;
            body += JoinTab(Ser(*r)) + '\n';
        }
        return body;
    };
    auto emit_attrdef = [&]() {
        std::string body;
        for (const auto* r : pg_attrdef_rows_) {
            if (r->oid < kFirstNormalObjectId)
                continue;
            body += JoinTab(Ser(*r)) + '\n';
        }
        return body;
    };
    auto emit_depend = [&]() {
        std::string body;
        for (const auto* r : pg_depend_rows_) {
            if (r->objid < kFirstNormalObjectId)
                continue;
            body += JoinTab(Ser(*r)) + '\n';
        }
        return body;
    };
    auto emit_statistic = [&]() {
        std::string body;
        for (const auto* r : pg_statistic_rows_) {
            if (r->starelid < kFirstNormalObjectId)
                continue;
            body += JoinTab(Ser(*r)) + '\n';
        }
        return body;
    };
    auto emit_inherits = [&]() {
        std::string body;
        for (const auto* r : pg_inherits_rows_) {
            if (r->inhrelid < kFirstNormalObjectId)
                continue;
            body += JoinTab(Ser(*r)) + '\n';
        }
        return body;
    };
    auto emit_am = [&]() {
        std::string body;
        for (const auto* r : pg_am_rows_) {
            if (r->oid < kFirstNormalObjectId)
                continue;
            body += JoinTab(Ser(*r)) + '\n';
        }
        return body;
    };
    auto emit_tablespace = [&]() {
        std::string body;
        for (const auto* r : pg_tablespace_rows_) {
            if (r->oid < kFirstNormalObjectId)
                continue;
            body += JoinTab(Ser(*r)) + '\n';
        }
        return body;
    };
    auto emit_trigger = [&]() {
        std::string body;
        for (const auto* r : pg_trigger_rows_) {
            if (r->oid < kFirstNormalObjectId)
                continue;
            body += JoinTab(Ser(*r)) + '\n';
        }
        return body;
    };
    auto emit_rewrite = [&]() {
        std::string body;
        for (const auto* r : pg_rewrite_rows_) {
            if (r->oid < kFirstNormalObjectId)
                continue;
            body += JoinTab(Ser(*r)) + '\n';
        }
        return body;
    };

    write_section("[pg_class]", emit_class());
    write_section("[pg_attribute]", emit_attr());
    write_section("[pg_type]", emit_type());
    write_section("[pg_operator]", emit_op());
    write_section("[pg_proc]", emit_proc());
    write_section("[pg_cast]", emit_cast());
    write_section("[pg_aggregate]", emit_agg());
    write_section("[pg_collation]", emit_coll());
    write_section("[pg_namespace]", emit_namespace());
    write_section("[pg_database]", emit_database());
    write_section("[pg_index]", emit_index());
    write_section("[pg_constraint]", emit_constraint());
    write_section("[pg_attrdef]", emit_attrdef());
    write_section("[pg_depend]", emit_depend());
    write_section("[pg_statistic]", emit_statistic());
    write_section("[pg_inherits]", emit_inherits());
    write_section("[pg_am]", emit_am());
    write_section("[pg_tablespace]", emit_tablespace());
    write_section("[pg_trigger]", emit_trigger());
    write_section("[pg_rewrite]", emit_rewrite());

    out.flush();
    return out.good();
}

bool Catalog::Load(const std::string& path) {
    std::ifstream in(path, std::ios::in);
    if (!in.is_open())
        return false;  // missing file = fresh initdb

    std::string line;
    if (!std::getline(in, line) || line != kCatalogMagic)
        return false;

    Oid saved_next_oid = kFirstNormalObjectId;
    if (!std::getline(in, line))
        return false;
    {
        auto fields = SplitTab(line);
        if (fields.size() < 2 || fields[0] != "next_oid" || !ParseU32(fields[1], saved_next_oid))
            return false;
    }

    // Read rows grouped under their section header. Rows carry their own OID
    // (>= kFirstNormalObjectId), so the Insert* methods preserve it without
    // calling AllocateOid; next_oid_ is restored once at the end.
    std::string section;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        if (line[0] == '[') {
            section = line;
            continue;
        }
        auto fields = SplitTab(line);
        if (fields.empty())
            continue;

        if (section == "[pg_class]") {
            if (auto* r = DeserPgClass(fields))
                InsertClass(r);
        } else if (section == "[pg_attribute]") {
            if (auto* r = DeserPgAttribute(fields))
                InsertAttribute(r);
        } else if (section == "[pg_type]") {
            if (auto* r = DeserPgType(fields))
                InsertType(r);
        } else if (section == "[pg_operator]") {
            if (auto* r = DeserPgOperator(fields))
                InsertOperator(r);
        } else if (section == "[pg_proc]") {
            if (auto* r = DeserPgProc(fields))
                InsertProc(r);
        } else if (section == "[pg_cast]") {
            if (auto* r = DeserPgCast(fields))
                InsertCast(r);
        } else if (section == "[pg_aggregate]") {
            if (auto* r = DeserPgAggregate(fields))
                InsertAggregate(r);
        } else if (section == "[pg_collation]") {
            if (auto* r = DeserPgCollation(fields))
                InsertCollation(r);
        } else if (section == "[pg_namespace]") {
            if (auto* r = DeserPgNamespace(fields))
                InsertNamespace(r);
        } else if (section == "[pg_database]") {
            if (auto* r = DeserPgDatabase(fields))
                InsertDatabase(r);
        } else if (section == "[pg_index]") {
            if (auto* r = DeserPgIndex(fields))
                InsertIndex(r);
        } else if (section == "[pg_constraint]") {
            if (auto* r = DeserPgConstraint(fields))
                InsertConstraint(r);
        } else if (section == "[pg_attrdef]") {
            if (auto* r = DeserPgAttrdef(fields))
                InsertAttrdef(r);
        } else if (section == "[pg_depend]") {
            if (auto* r = DeserPgDepend(fields))
                InsertDepend(r);
        } else if (section == "[pg_statistic]") {
            if (auto* r = DeserPgStatistic(fields))
                InsertStatistic(r);
        } else if (section == "[pg_inherits]") {
            if (auto* r = DeserPgInherits(fields))
                InsertInherits(r);
        } else if (section == "[pg_am]") {
            if (auto* r = DeserPgAm(fields))
                InsertAm(r);
        } else if (section == "[pg_tablespace]") {
            if (auto* r = DeserPgTablespace(fields))
                InsertTablespace(r);
        } else if (section == "[pg_trigger]") {
            if (auto* r = DeserPgTrigger(fields))
                InsertTrigger(r);
        } else if (section == "[pg_rewrite]") {
            if (auto* r = DeserPgRewrite(fields))
                InsertRewrite(r);
        }
    }

    SetNextOid(saved_next_oid);
    return true;
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

}  // namespace pgcpp::catalog

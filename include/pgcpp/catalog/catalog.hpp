#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pgcpp::catalog {

// Oid — the fundamental object identifier type used throughout PostgreSQL.
// In C PostgreSQL this is a global typedef; in pgcpp it lives in the catalog
// namespace (see rules/04-naming-and-namespace.md).
using Oid = uint32_t;

// kInvalidOid — the "no object" sentinel value (PostgreSQL's InvalidOid).
constexpr Oid kInvalidOid = 0;

// First valid OID for user objects (PostgreSQL FirstNormalObjectId).
constexpr Oid kFirstNormalObjectId = 16384;

// Forward declarations of catalog row types (defined in pg_*.h).
struct FormData_pg_class;
struct FormData_pg_attribute;
struct FormData_pg_type;
struct FormData_pg_operator;
struct FormData_pg_proc;
struct FormData_pg_cast;
struct FormData_pg_aggregate;
struct FormData_pg_collation;

// Catalog — the in-memory system catalog registry.
//
// In PostgreSQL, system catalogs (pg_class, pg_attribute, pg_type, ...) are
// ordinary tables stored on disk and cached in the system cache (SysCache).
// For pgcpp's early phases we keep an in-memory representation populated
// during bootstrap, with the same row types and lookup semantics.
//
// Row storage uses palloc-allocated copies; the Catalog does not own the
// memory contexts of its rows (callers allocate in a long-lived context).
class Catalog {
public:
    Catalog() = default;
    ~Catalog() = default;

    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;

    // --- pg_class accessors ---

    // Insert a pg_class row. Returns the OID assigned (or rel.oid if set).
    Oid InsertClass(FormData_pg_class* row);
    // Look up a pg_class row by OID. Returns nullptr if not found.
    const FormData_pg_class* GetClassByOid(Oid oid) const;
    // Look up a pg_class row by relation name. Returns nullptr if not found.
    const FormData_pg_class* GetClassByName(const std::string& name) const;
    // Update a pg_class row identified by OID. Returns false if not found.
    bool UpdateClass(Oid oid, const FormData_pg_class* new_row);
    // Delete a pg_class row by OID. Returns false if not found.
    bool DeleteClass(Oid oid);
    // Number of pg_class rows.
    std::size_t ClassCount() const { return pg_class_rows_.size(); }

    // --- pg_attribute accessors ---

    // Insert a pg_attribute row.
    void InsertAttribute(FormData_pg_attribute* row);
    // Look up a pg_attribute by (relid, attnum). Returns nullptr if not found.
    const FormData_pg_attribute* GetAttribute(Oid relid, int16_t attnum) const;
    // All attributes for a given relation, ordered by attnum.
    std::vector<const FormData_pg_attribute*> GetAttributes(Oid relid) const;
    // Delete all attributes for a relation (used when dropping a relation).
    std::size_t DeleteAttributes(Oid relid);

    // --- pg_type accessors ---

    // Insert a pg_type row.
    Oid InsertType(FormData_pg_type* row);
    // Look up a pg_type row by OID.
    const FormData_pg_type* GetTypeByOid(Oid oid) const;
    // Look up a pg_type row by type name.
    const FormData_pg_type* GetTypeByName(const std::string& name) const;

    // --- pg_operator accessors ---

    // Insert a pg_operator row. Returns the OID (or row.oid if set).
    Oid InsertOperator(FormData_pg_operator* row);
    // Look up a pg_operator by OID.
    const FormData_pg_operator* GetOperatorByOid(Oid oid) const;
    // Look up operators by name (may return multiple with different arg types).
    std::vector<const FormData_pg_operator*> GetOperatorsByName(const std::string& name) const;
    // Look up an exact operator by (name, left_type, right_type).
    const FormData_pg_operator* GetOperator(const std::string& name, Oid left_type,
                                            Oid right_type) const;

    // --- pg_proc accessors ---

    // Insert a pg_proc row. Returns the OID (or row.oid if set).
    Oid InsertProc(FormData_pg_proc* row);
    // Look up a pg_proc by OID.
    const FormData_pg_proc* GetProcByOid(Oid oid) const;
    // Look up pg_proc entries by name (may return multiple with different arg types).
    std::vector<const FormData_pg_proc*> GetProcsByName(const std::string& name) const;

    // --- pg_cast accessors ---

    // Insert a pg_cast row. Returns the OID (or row.oid if set).
    Oid InsertCast(FormData_pg_cast* row);
    // Look up a cast by (source, target) type OIDs.
    const FormData_pg_cast* GetCast(Oid source_type, Oid target_type) const;
    // All casts from a given source type.
    std::vector<const FormData_pg_cast*> GetCastsBySource(Oid source_type) const;

    // --- pg_aggregate accessors ---

    // Insert a pg_aggregate row.
    void InsertAggregate(FormData_pg_aggregate* row);
    // Look up a pg_aggregate by aggfnoid (the pg_proc OID of the aggregate).
    const FormData_pg_aggregate* GetAggregate(Oid aggfnoid) const;

    // --- pg_collation accessors ---

    // Insert a pg_collation row. Returns the OID (or row.oid if set).
    Oid InsertCollation(FormData_pg_collation* row);
    // Look up a pg_collation by OID.
    const FormData_pg_collation* GetCollationByOid(Oid oid) const;
    // Look up a pg_collation by name.
    const FormData_pg_collation* GetCollationByName(const std::string& name) const;

    // --- OID assignment ---

    // Allocate the next OID (PostgreSQL GetNewOid equivalent for catalog).
    Oid AllocateOid();

private:
    std::vector<FormData_pg_class*> pg_class_rows_;
    std::vector<FormData_pg_attribute*> pg_attribute_rows_;
    std::vector<FormData_pg_type*> pg_type_rows_;
    std::vector<FormData_pg_operator*> pg_operator_rows_;
    std::vector<FormData_pg_proc*> pg_proc_rows_;
    std::vector<FormData_pg_cast*> pg_cast_rows_;
    std::vector<FormData_pg_aggregate*> pg_aggregate_rows_;
    std::vector<FormData_pg_collation*> pg_collation_rows_;
    Oid next_oid_ = kFirstNormalObjectId;
};

// Global catalog accessor. Returns the process-wide catalog instance.
// In PostgreSQL this is implicit (catalogs are tables); in pgcpp we use an
// explicit global until the storage layer is built (Phase 6).
Catalog* GetCatalog();
void SetCatalog(Catalog* catalog);

// --- CatalogTuple* API (PostgreSQL-compatible names) ---
//
// These operate on the global catalog. Row pointers must be palloc-allocated
// in a long-lived context; the catalog stores them as-is (no copy).

// Insert a pg_class row and return its OID.
Oid CatalogTupleInsert(FormData_pg_class* row);
// Update the pg_class row with the given OID. Returns false if not found.
bool CatalogTupleUpdate(Oid oid, const FormData_pg_class* new_row);
// Delete the pg_class row with the given OID. Returns false if not found.
bool CatalogTupleDelete(Oid oid);

}  // namespace pgcpp::catalog

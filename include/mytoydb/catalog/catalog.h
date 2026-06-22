#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mytoydb::catalog {

// Oid — the fundamental object identifier type used throughout PostgreSQL.
// In C PostgreSQL this is a global typedef; in MyToyDB it lives in the catalog
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

// Catalog — the in-memory system catalog registry.
//
// In PostgreSQL, system catalogs (pg_class, pg_attribute, pg_type, ...) are
// ordinary tables stored on disk and cached in the system cache (SysCache).
// For MyToyDB's early phases we keep an in-memory representation populated
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

    // --- OID assignment ---

    // Allocate the next OID (PostgreSQL GetNewOid equivalent for catalog).
    Oid AllocateOid();

private:
    std::vector<FormData_pg_class*> pg_class_rows_;
    std::vector<FormData_pg_attribute*> pg_attribute_rows_;
    std::vector<FormData_pg_type*> pg_type_rows_;
    Oid next_oid_ = kFirstNormalObjectId;
};

// Global catalog accessor. Returns the process-wide catalog instance.
// In PostgreSQL this is implicit (catalogs are tables); in MyToyDB we use an
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

}  // namespace mytoydb::catalog

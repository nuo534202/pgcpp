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
struct FormData_pg_namespace;
struct FormData_pg_database;
struct FormData_pg_index;
struct FormData_pg_constraint;
struct FormData_pg_attrdef;
struct FormData_pg_depend;
struct FormData_pg_statistic;
struct FormData_pg_inherits;
struct FormData_pg_am;
struct FormData_pg_tablespace;
struct FormData_pg_trigger;
struct FormData_pg_rewrite;

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

    // --- pg_namespace accessors ---

    Oid InsertNamespace(FormData_pg_namespace* row);
    const FormData_pg_namespace* GetNamespaceByOid(Oid oid) const;
    const FormData_pg_namespace* GetNamespaceByName(const std::string& name) const;
    bool DeleteNamespace(Oid oid);

    // --- pg_database accessors ---

    Oid InsertDatabase(FormData_pg_database* row);
    const FormData_pg_database* GetDatabaseByOid(Oid oid) const;
    const FormData_pg_database* GetDatabaseByName(const std::string& name) const;
    bool DeleteDatabase(Oid oid);

    // --- pg_index accessors ---

    Oid InsertIndex(FormData_pg_index* row);
    const FormData_pg_index* GetIndexByOid(Oid indexrelid) const;
    std::vector<const FormData_pg_index*> GetIndexesByRelid(Oid indrelid) const;
    bool DeleteIndex(Oid indexrelid);
    std::size_t DeleteIndexesForRelid(Oid indrelid);

    // --- pg_constraint accessors ---

    Oid InsertConstraint(FormData_pg_constraint* row);
    const FormData_pg_constraint* GetConstraintByOid(Oid oid) const;
    std::vector<const FormData_pg_constraint*> GetConstraintsByRelid(Oid conrelid) const;
    bool DeleteConstraint(Oid oid);
    std::size_t DeleteConstraintsForRelid(Oid conrelid);

    // --- pg_attrdef accessors ---

    Oid InsertAttrdef(FormData_pg_attrdef* row);
    const FormData_pg_attrdef* GetAttrdef(Oid adrelid, int16_t adnum) const;
    std::vector<const FormData_pg_attrdef*> GetAttrdefsByRelid(Oid adrelid) const;
    bool DeleteAttrdef(Oid oid);
    std::size_t DeleteAttrdefsForRelid(Oid adrelid);

    // --- pg_depend accessors ---

    void InsertDepend(FormData_pg_depend* row);
    std::vector<const FormData_pg_depend*> GetDependsByRef(Oid refclassid, Oid refobjid) const;
    std::vector<const FormData_pg_depend*> GetDependsByObj(Oid classid, Oid objid) const;
    std::size_t DeleteDependsByObj(Oid classid, Oid objid);
    std::size_t DeleteDependsByRef(Oid refclassid, Oid refobjid);

    // --- pg_statistic accessors ---

    void InsertStatistic(FormData_pg_statistic* row);
    const FormData_pg_statistic* GetStatistic(Oid starelid, int16_t staattnum) const;
    std::size_t DeleteStatisticsForRelid(Oid starelid);

    // --- pg_inherits accessors ---

    void InsertInherits(FormData_pg_inherits* row);
    std::vector<const FormData_pg_inherits*> GetInheritsByParent(Oid inhparent) const;
    const FormData_pg_inherits* GetInheritsByChild(Oid inhrelid) const;
    bool DeleteInherits(Oid inhrelid);

    // --- pg_am accessors ---

    Oid InsertAm(FormData_pg_am* row);
    const FormData_pg_am* GetAmByOid(Oid oid) const;
    const FormData_pg_am* GetAmByName(const std::string& name) const;

    // --- pg_tablespace accessors ---

    Oid InsertTablespace(FormData_pg_tablespace* row);
    const FormData_pg_tablespace* GetTablespaceByOid(Oid oid) const;
    const FormData_pg_tablespace* GetTablespaceByName(const std::string& name) const;
    bool DeleteTablespace(Oid oid);

    // --- pg_trigger accessors ---

    Oid InsertTrigger(FormData_pg_trigger* row);
    const FormData_pg_trigger* GetTriggerByOid(Oid oid) const;
    std::vector<const FormData_pg_trigger*> GetTriggersByRelid(Oid tgrelid) const;
    bool DeleteTrigger(Oid oid);
    std::size_t DeleteTriggersForRelid(Oid tgrelid);

    // --- pg_rewrite accessors ---

    Oid InsertRewrite(FormData_pg_rewrite* row);
    const FormData_pg_rewrite* GetRewriteByOid(Oid oid) const;
    std::vector<const FormData_pg_rewrite*> GetRewritesByRelid(Oid ev_class) const;
    bool DeleteRewrite(Oid oid);
    std::size_t DeleteRewritesForRelid(Oid ev_class);

    // --- OID assignment ---

    // Allocate the next OID (PostgreSQL GetNewOid equivalent for catalog).
    Oid AllocateOid();

    // --- Persistence (A-3) ---
    //
    // Save serializes user-created catalog rows (oid >= kFirstNormalObjectId)
    // and next_oid_ to a TSV file at `path`. Returns false on I/O error.
    // Load restores them, merging on top of BootstrapCatalog's built-in rows.
    // A missing file is not an error (returns false silently — fresh initdb).
    bool Save(const std::string& path) const;
    bool Load(const std::string& path);

    // Restore the OID counter after Load (avoids OID collisions).
    void SetNextOid(Oid oid) { next_oid_ = oid; }

    // --- P1-2: Transactional catalog (DDL rollback support) ---
    //
    // pgcpp does not implement per-row MVCC on catalog tables. Instead, the
    // transaction system takes a deep-copy snapshot of all user-created
    // catalog rows at transaction start. On ROLLBACK, the snapshot is restored
    // (undoing all DDL changes). On COMMIT, the catalog is persisted to disk
    // if any DDL marked it dirty.
    //
    // Limitations (documented):
    //   - SAVEPOINT-level DDL rollback is not supported (only top-level).
    //   - Crash recovery uses on-commit Save() rather than WAL replay.

    // Set the path used by CommitDirty() to persist the catalog. Called once
    // at server startup after Load(). If empty (default), CommitDirty is a
    // no-op (used by tests that don't need persistence).
    void SetPersistPath(const std::string& path) { persist_path_ = path; }

    // Take a deep-copy snapshot of all user-created rows. Called at
    // StartTransaction. If a snapshot already exists, it is discarded first.
    void TakeSnapshot();

    // Restore the catalog to the snapshot state, undoing all modifications
    // made since TakeSnapshot. Frees all user-created rows added since the
    // snapshot, and deep-copies the snapshot rows back into the live vectors.
    // No-op if no snapshot exists.
    void RestoreSnapshot();

    // Discard the snapshot without restoring (used on COMMIT).
    void DiscardSnapshot();

    // True if a snapshot is currently held.
    bool HasSnapshot() const { return snapshot_ != nullptr; }

    // Mark the catalog as modified (called by every DDL write method via
    // PreWrite()). CommitDirty checks this flag to decide whether to Save.
    void MarkDirty() { dirty_ = true; }
    bool IsDirty() const { return dirty_; }
    void ClearDirty() { dirty_ = false; }

    // If dirty_, call Save(persist_path_) and clear dirty_. Called at
    // CommitTransaction. No-op if not dirty or persist_path_ is empty.
    void CommitDirty();

private:
    // PreWrite — called at the start of every DDL write method. Ensures a
    // snapshot exists (so ROLLBACK can undo this change) and marks the
    // catalog dirty (so COMMIT persists it).
    void PreWrite();

    std::vector<FormData_pg_class*> pg_class_rows_;
    std::vector<FormData_pg_attribute*> pg_attribute_rows_;
    std::vector<FormData_pg_type*> pg_type_rows_;
    std::vector<FormData_pg_operator*> pg_operator_rows_;
    std::vector<FormData_pg_proc*> pg_proc_rows_;
    std::vector<FormData_pg_cast*> pg_cast_rows_;
    std::vector<FormData_pg_aggregate*> pg_aggregate_rows_;
    std::vector<FormData_pg_collation*> pg_collation_rows_;
    std::vector<FormData_pg_namespace*> pg_namespace_rows_;
    std::vector<FormData_pg_database*> pg_database_rows_;
    std::vector<FormData_pg_index*> pg_index_rows_;
    std::vector<FormData_pg_constraint*> pg_constraint_rows_;
    std::vector<FormData_pg_attrdef*> pg_attrdef_rows_;
    std::vector<FormData_pg_depend*> pg_depend_rows_;
    std::vector<FormData_pg_statistic*> pg_statistic_rows_;
    std::vector<FormData_pg_inherits*> pg_inherits_rows_;
    std::vector<FormData_pg_am*> pg_am_rows_;
    std::vector<FormData_pg_tablespace*> pg_tablespace_rows_;
    std::vector<FormData_pg_trigger*> pg_trigger_rows_;
    std::vector<FormData_pg_rewrite*> pg_rewrite_rows_;
    Oid next_oid_ = kFirstNormalObjectId;

    // P1-2: transactional catalog state.
    struct CatalogSnapshot;
    CatalogSnapshot* snapshot_ = nullptr;  // owned; null = no snapshot
    bool dirty_ = false;
    std::string persist_path_;
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

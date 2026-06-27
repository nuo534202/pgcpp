// rel.h — Relation data structure bridging catalog and storage.
//
// Converted from PostgreSQL 15's src/include/utils/rel.h.
//
// A Relation is the in-memory handle for an open table, index, or sequence.
// It combines:
//   - Catalog metadata (pg_class row: name, kind, persistence, relfilenode)
//   - Tuple descriptor (array of pg_attribute rows describing columns)
//   - Storage handle (SmgrRelation for physical file I/O)
//   - Reference count (for open/close management)
//
// In PostgreSQL, RelationData is cached in a relcache. MyToyDB uses a simpler
// model: relations are opened on demand and closed explicitly. The relcache
// is a small map from OID to RelationData.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mytoydb/catalog/catalog.hpp"
#include "mytoydb/catalog/pg_attribute.hpp"
#include "mytoydb/catalog/pg_class.hpp"
#include "mytoydb/common/memory/memory_context.hpp"
#include "mytoydb/storage/relfilenode.hpp"
#include "mytoydb/storage/smgr.hpp"

namespace mytoydb::access {

// AttrDefault — a column DEFAULT expression (PG pg_attrdef row, simplified).
struct AttrDefault {
    int16_t adnum = 0;  // column number (1-based)
    std::string adbin;  // expression text (node-tree string)
};

// CheckConstraint — a CHECK constraint expression (PG pg_constraint row).
struct CheckConstraint {
    std::string ccname;  // constraint name
    std::string ccbin;   // expression text
    bool ccvalid = true;
    bool ccnoinherit = false;
};

// TupleConstr — per-descriptor constraint metadata (PG TupleConstrData).
struct TupleConstr {
    std::vector<AttrDefault> defval;     // column default expressions
    std::vector<CheckConstraint> check;  // CHECK constraints
    bool has_not_null = false;           // any column has NOT NULL?
};

// TupleDescData — tuple descriptor (array of column descriptions).
//
// In PostgreSQL this is TupleDescData with a flexible array of Form_pg_attribute.
// In C++ we use a vector for simplicity. The descriptor is reference-counted
// via the owning Relation.
struct TupleDescData {
    int natts = 0;  // number of user attributes
    std::vector<mytoydb::catalog::FormData_pg_attribute> attrs;

    // --- P0 extensions (Task 15.8.2 / GAP-M8-F03) ---
    TupleConstr constr;  // constraints (defaults + CHECKs)
    mytoydb::catalog::Oid tdtypeid = mytoydb::catalog::kInvalidOid;  // composite type OID
    int32_t tdtypmod = -1;                                           // type modifier
    bool tdhasoid = false;  // has OID column (MyToyDB: always false)
    int tdrefcount = 0;     // reference count (0 => may free)

    // Convenience accessor.
    const mytoydb::catalog::FormData_pg_attribute* Attr(int attnum) const {
        // attnum is 1-based for user attributes.
        if (attnum < 1 || attnum > natts)
            return nullptr;
        return &attrs[attnum - 1];
    }
};

// TupleDesc — pointer to a TupleDescData (PostgreSQL convention).
using TupleDesc = TupleDescData*;

// RelationData — the in-memory handle for an open relation.
//
// Fields mirror PostgreSQL's RelationData:
//   rd_id      — relation OID (pg_class.oid)
//   rd_rel     — pointer to the pg_class catalog row (not owned)
//   rd_smgr    — storage manager handle (owned by the smgr hash table)
//   rd_att     — tuple descriptor (owned by the relation)
//   rd_refcnt  — reference count for open/close
//   rd_isnailed — true for pinned catalog relations (not evicted)
//   rd_isvalid  — true if the relcache entry is fully initialized
struct RelationData {
    mytoydb::catalog::Oid rd_id = mytoydb::catalog::kInvalidOid;
    const mytoydb::catalog::FormData_pg_class* rd_rel = nullptr;
    mytoydb::storage::SmgrRelation rd_smgr = nullptr;
    TupleDesc rd_att = nullptr;
    int rd_refcnt = 0;
    bool rd_isnailed = false;
    bool rd_isvalid = false;

    ~RelationData();
};

// Relation — pointer to a RelationData (PostgreSQL convention).
using Relation = RelationData*;

// --- Relation lifecycle ---

// RelationOpen — open a relation by OID.
// Looks up the pg_class row and pg_attribute rows from the catalog, builds
// a RelationData with a tuple descriptor, and opens the storage manager
// handle. The reference count starts at 1.
// Returns nullptr if the relation does not exist.
Relation RelationOpen(mytoydb::catalog::Oid relid);

// RelationClose — decrement the reference count and free if it reaches 0.
void RelationClose(Relation relation);

// RelationGetSmgr — return the SmgrRelation, opening it lazily if needed.
mytoydb::storage::SmgrRelation RelationGetSmgr(Relation relation);

// --- PG-compatible aliases (relcache API) ---

// RelationIdGetRelation — PG alias for RelationOpen.
Relation RelationIdGetRelation(mytoydb::catalog::Oid relid);

// RelationCloseByOid — close a relation identified by OID (decrement refcnt).
// No-op if the OID is not currently cached.
void RelationCloseByOid(mytoydb::catalog::Oid relid);

// RelationBuildDesc — build a fresh RelationData from the catalog without
// consulting the relcache. This is the cache-miss path used internally by
// RelationOpen. The returned Relation has rd_refcnt = 1 and is owned by the
// caller (the relcache may also take ownership via RelationOpen). Returns
// nullptr if the catalog has no pg_class row for relid.
Relation RelationBuildDesc(mytoydb::catalog::Oid relid);

// RelationCacheInvalidate — drop the relcache entry for the given OID.
// Decrements the refcnt by 1 (matching PG's "invalidation drops one pin"
// semantics). The relation is removed from the cache and any storage handle
// is closed. Safe to call with an OID that is not cached (no-op).
void RelationCacheInvalidate(mytoydb::catalog::Oid relid);

// RelationClearRelation — invalidate a specific Relation in the cache.
// Decrements its refcnt and removes the entry. If the relation is not in
// the cache, only the refcnt is decremented.
void RelationClearRelation(Relation rel);

// RelationGetNumberOfAttributes — return the number of user attributes
// (rd_att->natts) of the relation.
int RelationGetNumberOfAttributes(Relation rel);

// --- Storage creation / destruction ---

// RelationCreateStorage — create the physical storage file for a relation.
// Uses the relfilenode from pg_class to build a RelFileNode and calls
// smgrcreate. The relation must already have a pg_class entry.
void RelationCreateStorage(mytoydb::catalog::Oid relfilenode, bool is_temp);

// RelationDropStorage — drop the physical storage for a relation.
// Closes the smgr handle and removes the file. Does not remove the
// catalog entry (that's the caller's responsibility).
void RelationDropStorage(Relation relation);

// RelationExtendStorage — extend the relation's main fork by one block,
// writing the provided page data. Used by heap_insert when a new page
// is needed.
void RelationExtendStorage(Relation relation, mytoydb::storage::BlockNumber block_num,
                           const char* page_data);

// RelationGetNumberOfBlocks — return the number of blocks in the main fork.
mytoydb::storage::BlockNumber RelationGetNumberOfBlocks(Relation relation);

// --- Tuple descriptor construction ---

// CreateTupleDesc — build a TupleDesc from a list of attributes.
// The attributes are copied into the descriptor. natts is set from the
// vector size.
TupleDesc CreateTupleDesc(const std::vector<mytoydb::catalog::FormData_pg_attribute>& attrs);

// --- tupdesc.c P0 extensions (Task 15.8.2 / GAP-M8-F03) ---

// CreateTemplateTupleDesc — allocate a descriptor with `natts` empty slots.
// The attrs vector is resized to natts; each slot is default-constructed.
TupleDesc CreateTemplateTupleDesc(int natts);

// CreateTupleDescCopy — deep-copy a descriptor (attrs only, no constraints).
TupleDesc CreateTupleDescCopy(TupleDesc tupdesc);

// CreateTupleDescCopyConstr — deep-copy a descriptor including constraints.
TupleDesc CreateTupleDescCopyConstr(TupleDesc tupdesc);

// TupleDescCopyEntry — copy a single attr slot from src to dst.
// dst_attnum and src_attnum are 1-based.
void TupleDescCopyEntry(TupleDesc dst, int dst_attnum, TupleDesc src, int src_attnum);

// FreeTupleDesc — reference-count-aware release. Decrements tdrefcount and
// frees the descriptor (via destroyPallocNode) when it reaches 0.
void FreeTupleDesc(TupleDesc tupdesc);

// equalTupleDescs — structural equality (attrs + constr + type metadata).
bool equalTupleDescs(TupleDesc tupdesc1, TupleDesc tupdesc2);

// TupleDescInitEntry — initialize attr slot `attnum` (1-based) with the given
// name and type. Type metadata (typlen/attbyval/attalign) is resolved via the
// catalog's pg_type; common built-in types fall back to hardcoded metadata
// when the catalog is not populated.
void TupleDescInitEntry(TupleDesc desc, int attnum, const std::string& name,
                        mytoydb::catalog::Oid type_oid, int32_t typmod, int attdim);

// TupleDescInitEntryCollation — set the collation of attr slot `attnum`.
void TupleDescInitEntryCollation(TupleDesc desc, int attnum, mytoydb::catalog::Oid collation);

// --- Relcache management ---

// InitializeRelcache — set up the relation cache (called at startup).
void InitializeRelcache();

// ResetRelcache — clear all cached relations (for testing).
void ResetRelcache();

}  // namespace mytoydb::access

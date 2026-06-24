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

#include "mytoydb/catalog/catalog.h"
#include "mytoydb/catalog/pg_attribute.h"
#include "mytoydb/catalog/pg_class.h"
#include "mytoydb/common/memory/memory_context.h"
#include "mytoydb/storage/relfilenode.h"
#include "mytoydb/storage/smgr.h"

namespace mytoydb::access {

// TupleDescData — tuple descriptor (array of column descriptions).
//
// In PostgreSQL this is TupleDescData with a flexible array of Form_pg_attribute.
// In C++ we use a vector for simplicity. The descriptor is reference-counted
// via the owning Relation.
struct TupleDescData {
    int natts = 0;  // number of user attributes
    std::vector<mytoydb::catalog::FormData_pg_attribute> attrs;

    // Convenience accessor.
    const mytoydb::catalog::FormData_pg_attribute* Attr(int attnum) const {
        // attnum is 1-based for user attributes.
        if (attnum < 1 || attnum > natts) return nullptr;
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

    ~RelationData() {
        // The tuple descriptor is palloc'd and owned by the relation.
        if (rd_att != nullptr) {
            rd_att->~TupleDescData();
            mytoydb::memory::pfree(rd_att);
            rd_att = nullptr;
        }
    }
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

// --- Storage creation / destruction ---

// RelationCreateStorage — create the physical storage file for a relation.
// Uses the relfilenode from pg_class to build a RelFileNode and calls
// smgrcreate. The relation must already have a pg_class entry.
void RelationCreateStorage(mytoydb::catalog::Oid relfilenode,
                           bool is_temp);

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
TupleDesc CreateTupleDesc(
    const std::vector<mytoydb::catalog::FormData_pg_attribute>& attrs);

// --- Relcache management ---

// InitializeRelcache — set up the relation cache (called at startup).
void InitializeRelcache();

// ResetRelcache — clear all cached relations (for testing).
void ResetRelcache();

}  // namespace mytoydb::access

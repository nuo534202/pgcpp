// rel.cpp — Relation data structure implementation.
//
// Converted from PostgreSQL 15's src/backend/utils/cache/relcache.c.
//
// Manages the relation cache: a map from OID to RelationData. Relations
// are opened by looking up pg_class and pg_attribute from the catalog,
// building a tuple descriptor, and opening the storage manager handle.
//
// MyToyDB simplifications:
//   - No shared cache invalidation (single-process)
//   - No catalog index lookups (direct catalog scan)
//   - Reference counting is manual (no automatic eviction)

#include "mytoydb/access/rel.h"

#include <string>
#include <unordered_map>

#include "mytoydb/catalog/catalog.h"
#include "mytoydb/common/error/elog.h"
#include "mytoydb/common/memory/memory_context.h"
#include "mytoydb/storage/bufmgr.h"
#include "mytoydb/storage/smgr.h"

namespace mytoydb::access {

namespace {

// Relcache: OID → RelationData. Uses palloc'd RelationData pointers.
// The relcache owns the RelationData memory.
std::unordered_map<mytoydb::catalog::Oid, Relation>& Relcache() {
    static std::unordered_map<mytoydb::catalog::Oid, Relation> cache;
    return cache;
}

// Build a RelFileNodeBackend from a relfilenode OID.
// Uses default tablespace (0) and database (16384) for MyToyDB.
mytoydb::storage::RelFileNodeBackend MakeRelFileNodeBackend(
    mytoydb::catalog::Oid relfilenode) {
    mytoydb::storage::RelFileNodeBackend rnode;
    rnode.node.spc_node = 0;       // default tablespace
    rnode.node.db_node = 16384;    // default database OID
    rnode.node.rel_node = relfilenode;
    rnode.backend = 0;             // not a temp relation
    return rnode;
}

}  // namespace

// --- Tuple descriptor construction ---

TupleDesc CreateTupleDesc(
    const std::vector<mytoydb::catalog::FormData_pg_attribute>& attrs) {
    void* mem = mytoydb::memory::palloc(sizeof(TupleDescData));
    TupleDesc desc = new (mem) TupleDescData();
    desc->natts = static_cast<int>(attrs.size());
    desc->attrs = attrs;
    return desc;
}

// --- Relation lifecycle ---

Relation RelationOpen(mytoydb::catalog::Oid relid) {
    // Check the relcache first.
    auto& cache = Relcache();
    auto it = cache.find(relid);
    if (it != cache.end()) {
        it->second->rd_refcnt++;
        return it->second;
    }

    // Look up the pg_class row.
    mytoydb::catalog::Catalog* cat = mytoydb::catalog::GetCatalog();
    const mytoydb::catalog::FormData_pg_class* pg_class =
        cat->GetClassByOid(relid);
    if (pg_class == nullptr) {
        return nullptr;
    }

    // Build the tuple descriptor from pg_attribute rows.
    std::vector<const mytoydb::catalog::FormData_pg_attribute*> attr_ptrs =
        cat->GetAttributes(relid);
    std::vector<mytoydb::catalog::FormData_pg_attribute> attrs;
    attrs.reserve(attr_ptrs.size());
    for (const auto* attr : attr_ptrs) {
        attrs.push_back(*attr);
    }

    // Allocate the RelationData.
    void* mem = mytoydb::memory::palloc(sizeof(RelationData));
    Relation rel = new (mem) RelationData();
    rel->rd_id = relid;
    rel->rd_rel = pg_class;
    rel->rd_att = CreateTupleDesc(attrs);
    rel->rd_refcnt = 1;
    rel->rd_isnailed = false;
    rel->rd_isvalid = true;

    // Open the storage manager handle (lazy: file is not created here).
    mytoydb::catalog::Oid relfilenode = pg_class->relfilenode;
    if (relfilenode != mytoydb::catalog::kInvalidOid) {
        rel->rd_smgr = mytoydb::storage::smgropen(
            MakeRelFileNodeBackend(relfilenode));
    }

    cache[relid] = rel;
    return rel;
}

void RelationClose(Relation relation) {
    if (relation == nullptr) return;
    relation->rd_refcnt--;
    // We keep the relation in the relcache even at refcnt 0 for reuse.
    // The relcache is cleared explicitly by ResetRelcache().
}

mytoydb::storage::SmgrRelation RelationGetSmgr(Relation relation) {
    if (relation->rd_smgr == nullptr) {
        mytoydb::catalog::Oid relfilenode = relation->rd_rel->relfilenode;
        if (relfilenode == mytoydb::catalog::kInvalidOid) {
            ereport(mytoydb::error::LogLevel::kError,
                    "relation " + std::to_string(relation->rd_id) +
                    " has no relfilenode");
        }
        relation->rd_smgr = mytoydb::storage::smgropen(
            MakeRelFileNodeBackend(relfilenode));
    }
    return relation->rd_smgr;
}

// --- Storage creation / destruction ---

void RelationCreateStorage(mytoydb::catalog::Oid relfilenode, bool is_temp) {
    mytoydb::storage::RelFileNodeBackend rnode =
        MakeRelFileNodeBackend(relfilenode);
    if (is_temp) {
        rnode.backend = 1;  // MyBackendId
    }
    mytoydb::storage::SmgrRelation srel = mytoydb::storage::smgropen(rnode);
    mytoydb::storage::smgrcreate(srel, mytoydb::storage::ForkNumber::kMain, false);
}

void RelationDropStorage(Relation relation) {
    if (relation->rd_smgr != nullptr) {
        // Flush and close all buffers for this relation.
        mytoydb::storage::RelFileNode rnode = relation->rd_smgr->smgr_rnode.node;
        mytoydb::storage::DropRelationBuffers(rnode);
        mytoydb::storage::smgrclose(relation->rd_smgr);
        relation->rd_smgr = nullptr;
    }
}

void RelationExtendStorage(Relation relation,
                           mytoydb::storage::BlockNumber block_num,
                           const char* page_data) {
    mytoydb::storage::SmgrRelation srel = RelationGetSmgr(relation);
    mytoydb::storage::smgrextend(srel, mytoydb::storage::ForkNumber::kMain,
                                 block_num, page_data, false);
}

mytoydb::storage::BlockNumber RelationGetNumberOfBlocks(Relation relation) {
    mytoydb::storage::SmgrRelation srel = RelationGetSmgr(relation);
    return mytoydb::storage::smgrnblocks(srel, mytoydb::storage::ForkNumber::kMain);
}

// --- Relcache management ---

void InitializeRelcache() {
    Relcache().clear();
}

void ResetRelcache() {
    auto& cache = Relcache();
    for (auto& [oid, rel] : cache) {
        // Call destructor and free memory.
        rel->~RelationData();
        mytoydb::memory::pfree(rel);
    }
    cache.clear();
}

}  // namespace mytoydb::access

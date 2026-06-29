// rel.cpp — Relation data structure implementation.
//
// Converted from PostgreSQL 15's src/backend/utils/cache/relcache.c.
//
// Manages the relation cache: a map from OID to RelationData. Relations
// are opened by looking up pg_class and pg_attribute from the catalog,
// building a tuple descriptor, and opening the storage manager handle.
//
// pgcpp simplifications:
//   - No shared cache invalidation (single-process)
//   - No catalog index lookups (direct catalog scan)
//   - Reference counting is manual (no automatic eviction)

#include "access/rel.hpp"

#include <string>
#include <unordered_map>

#include "catalog/catalog.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/memory_context.hpp"
#include "storage/bufmgr.hpp"
#include "storage/smgr.hpp"

namespace pgcpp::access {
using pgcpp::nodes::destroyPallocNode;
using pgcpp::nodes::makePallocNode;

namespace {

// Relcache: OID → RelationData. Uses palloc'd RelationData pointers.
// The relcache owns the RelationData memory.
std::unordered_map<pgcpp::catalog::Oid, Relation>& Relcache() {
    static std::unordered_map<pgcpp::catalog::Oid, Relation> cache;
    return cache;
}

// Build a RelFileNodeBackend from a relfilenode OID.
// Uses default tablespace (0) and database (16384) for pgcpp.
pgcpp::storage::RelFileNodeBackend MakeRelFileNodeBackend(pgcpp::catalog::Oid relfilenode) {
    pgcpp::storage::RelFileNodeBackend rnode;
    rnode.node.spc_node = 0;     // default tablespace
    rnode.node.db_node = 16384;  // default database OID
    rnode.node.rel_node = relfilenode;
    rnode.backend = 0;  // not a temp relation
    return rnode;
}

}  // namespace

// --- RelationData destructor ---
//
// Defined here (not inline in rel.h) so it can call destroyPallocNode,
// which requires node.h. The tuple descriptor is palloc'd via
// makePallocNode and owned by the relation; destroying it via
// destroyPallocNode unregisters its destructor entry to prevent
// double-free when the MemoryContext is later deleted.
RelationData::~RelationData() {
    if (rd_att != nullptr) {
        destroyPallocNode(rd_att);
        rd_att = nullptr;
    }
}

// --- Tuple descriptor construction ---

TupleDesc CreateTupleDesc(const std::vector<pgcpp::catalog::FormData_pg_attribute>& attrs) {
    TupleDesc desc = makePallocNode<TupleDescData>();
    desc->natts = static_cast<int>(attrs.size());
    desc->attrs = attrs;
    return desc;
}

// --- Relation lifecycle ---

// RelationBuildDesc — build a fresh RelationData from the catalog (cache-miss
// path). Extracted from RelationOpen so it can be tested independently and
// reused by invalidation/refresh paths.
Relation RelationBuildDesc(pgcpp::catalog::Oid relid) {
    pgcpp::catalog::Catalog* cat = pgcpp::catalog::GetCatalog();
    if (cat == nullptr) {
        return nullptr;
    }
    const pgcpp::catalog::FormData_pg_class* pg_class = cat->GetClassByOid(relid);
    if (pg_class == nullptr) {
        return nullptr;
    }

    // Build the tuple descriptor from pg_attribute rows.
    std::vector<const pgcpp::catalog::FormData_pg_attribute*> attr_ptrs = cat->GetAttributes(relid);
    std::vector<pgcpp::catalog::FormData_pg_attribute> attrs;
    attrs.reserve(attr_ptrs.size());
    for (const auto* attr : attr_ptrs) {
        attrs.push_back(*attr);
    }

    // Allocate the RelationData.
    Relation rel = makePallocNode<RelationData>();
    rel->rd_id = relid;
    rel->rd_rel = pg_class;
    rel->rd_att = CreateTupleDesc(attrs);
    rel->rd_refcnt = 1;
    rel->rd_isnailed = false;
    rel->rd_isvalid = true;

    // Open the storage manager handle (lazy: file is not created here).
    pgcpp::catalog::Oid relfilenode = pg_class->relfilenode;
    if (relfilenode != pgcpp::catalog::kInvalidOid) {
        rel->rd_smgr = pgcpp::storage::smgropen(MakeRelFileNodeBackend(relfilenode));
    }
    return rel;
}

Relation RelationOpen(pgcpp::catalog::Oid relid) {
    // Check the relcache first.
    auto& cache = Relcache();
    auto it = cache.find(relid);
    if (it != cache.end()) {
        it->second->rd_refcnt++;
        return it->second;
    }

    // Cache miss: build a fresh descriptor and install it in the cache.
    Relation rel = RelationBuildDesc(relid);
    if (rel == nullptr) {
        return nullptr;
    }
    cache[relid] = rel;
    return rel;
}

void RelationClose(Relation relation) {
    if (relation == nullptr)
        return;
    relation->rd_refcnt--;
    // We keep the relation in the relcache even at refcnt 0 for reuse.
    // The relcache is cleared explicitly by ResetRelcache().
}

Relation RelationIdGetRelation(pgcpp::catalog::Oid relid) {
    return RelationOpen(relid);
}

void RelationCloseByOid(pgcpp::catalog::Oid relid) {
    auto& cache = Relcache();
    auto it = cache.find(relid);
    if (it == cache.end()) {
        return;
    }
    RelationClose(it->second);
}

void RelationCacheInvalidate(pgcpp::catalog::Oid relid) {
    auto& cache = Relcache();
    auto it = cache.find(relid);
    if (it == cache.end()) {
        return;
    }
    Relation rel = it->second;
    // Drop one pin (PG semantics: invalidation removes one reference). The
    // entry is removed regardless of remaining refcnt — callers are expected
    // to re-open the relation if they still hold a stale pointer.
    if (rel->rd_refcnt > 0) {
        rel->rd_refcnt--;
    }
    // Close the storage handle if one was opened.
    if (rel->rd_smgr != nullptr) {
        pgcpp::storage::smgrclose(rel->rd_smgr);
        rel->rd_smgr = nullptr;
    }
    cache.erase(it);
}

void RelationClearRelation(Relation rel) {
    if (rel == nullptr) {
        return;
    }
    auto& cache = Relcache();
    auto it = cache.find(rel->rd_id);
    if (it != cache.end() && it->second == rel) {
        if (rel->rd_refcnt > 0) {
            rel->rd_refcnt--;
        }
        if (rel->rd_smgr != nullptr) {
            pgcpp::storage::smgrclose(rel->rd_smgr);
            rel->rd_smgr = nullptr;
        }
        cache.erase(it);
    } else {
        // Not in the cache (or pointer mismatch): just decrement the refcnt.
        if (rel->rd_refcnt > 0) {
            rel->rd_refcnt--;
        }
    }
}

int RelationGetNumberOfAttributes(Relation rel) {
    if (rel == nullptr || rel->rd_att == nullptr) {
        return 0;
    }
    return rel->rd_att->natts;
}

pgcpp::storage::SmgrRelation RelationGetSmgr(Relation relation) {
    if (relation->rd_smgr == nullptr) {
        pgcpp::catalog::Oid relfilenode = relation->rd_rel->relfilenode;
        if (relfilenode == pgcpp::catalog::kInvalidOid) {
            ereport(pgcpp::error::LogLevel::kError,
                    "relation " + std::to_string(relation->rd_id) + " has no relfilenode");
        }
        relation->rd_smgr = pgcpp::storage::smgropen(MakeRelFileNodeBackend(relfilenode));
    }
    return relation->rd_smgr;
}

// --- Storage creation / destruction ---

void RelationCreateStorage(pgcpp::catalog::Oid relfilenode, bool is_temp) {
    pgcpp::storage::RelFileNodeBackend rnode = MakeRelFileNodeBackend(relfilenode);
    if (is_temp) {
        rnode.backend = 1;  // MyBackendId
    }
    pgcpp::storage::SmgrRelation srel = pgcpp::storage::smgropen(rnode);
    pgcpp::storage::smgrcreate(srel, pgcpp::storage::ForkNumber::kMain, false);
}

void RelationDropStorage(Relation relation) {
    if (relation->rd_smgr != nullptr) {
        // Flush and close all buffers for this relation.
        pgcpp::storage::RelFileNode rnode = relation->rd_smgr->smgr_rnode.node;
        pgcpp::storage::DropRelationBuffers(rnode);
        pgcpp::storage::smgrclose(relation->rd_smgr);
        relation->rd_smgr = nullptr;
    }
}

void RelationExtendStorage(Relation relation, pgcpp::storage::BlockNumber block_num,
                           const char* page_data) {
    pgcpp::storage::SmgrRelation srel = RelationGetSmgr(relation);
    pgcpp::storage::smgrextend(srel, pgcpp::storage::ForkNumber::kMain, block_num, page_data,
                               false);
}

pgcpp::storage::BlockNumber RelationGetNumberOfBlocks(Relation relation) {
    pgcpp::storage::SmgrRelation srel = RelationGetSmgr(relation);
    return pgcpp::storage::smgrnblocks(srel, pgcpp::storage::ForkNumber::kMain);
}

// --- Relcache management ---

void InitializeRelcache() {
    Relcache().clear();
}

void ResetRelcache() {
    auto& cache = Relcache();
    for (auto& [oid, rel] : cache) {
        destroyPallocNode(rel);
    }
    cache.clear();
}

}  // namespace pgcpp::access

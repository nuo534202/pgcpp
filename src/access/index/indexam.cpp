// indexam.cpp — Generic index access method dispatch.
//
// Converted from PostgreSQL 15's src/backend/access/index/indexam.c.
//
// Implements LookupAmRoutine (resolves an AM OID to its IndexAmRoutine table)
// and the generic index_* entry points that dispatch to the AM's routines.
// pgcpp only implements the B-tree AM (kBTreeAmOid = 403); other AM OIDs
// return nullptr from LookupAmRoutine and the generic entry points ereport.

#include "access/indexam.hpp"

#include <string>

#include "access/brin.hpp"
#include "access/gin.hpp"
#include "access/gist.hpp"
#include "access/hash.hpp"
#include "access/nbtree.hpp"
#include "access/spgist.hpp"
#include "catalog/catalog.hpp"
#include "common/error/elog.hpp"
#include "storage/bufmgr.hpp"

namespace pgcpp::access {
using pgcpp::catalog::kInvalidOid;
using pgcpp::catalog::Oid;
using pgcpp::storage::Buffer;
using pgcpp::transaction::ItemPointerData;

namespace {

// The B-tree AM routine table.
const IndexAmRoutine kBtreeAmRoutine = {
    kBTreeAmOid, &btcanreturn, &btinsert, &btbeginscan, &btgettuple,
    &btrescan,   &btendscan,   &btbuild,  &btgetbitmap,
};

// The hash AM routine table.
const IndexAmRoutine kHashAmRoutine = {
    kHashAmOid,  &hashcanreturn, &hashinsert, &hashbeginscan, &hashgettuple,
    &hashrescan, &hashendscan,   &hashbuild,  &hashgetbitmap,
};

// The GIN AM routine table.
const IndexAmRoutine kGinAmRoutine = {
    kGinAmOid,  &gincanreturn, &gininsert, &ginbeginscan, &gingettuple,
    &ginrescan, &ginendscan,   &ginbuild,  &gingetbitmap,
};

// The GiST AM routine table.
const IndexAmRoutine kGistAmRoutine = {
    kGistAmOid,  &gistcanreturn, &gistinsert, &gistbeginscan, &gistgettuple,
    &gistrescan, &gistendscan,   &gistbuild,  &gistgetbitmap,
};

// The BRIN AM routine table.
const IndexAmRoutine kBrinAmRoutine = {
    kBrinAmOid,  &brincanreturn, &brininsert, &brinbeginscan, &bringettuple,
    &brinrescan, &brinendscan,   &brinbuild,  &bringetbitmap,
};

// The SP-GiST AM routine table.
const IndexAmRoutine kSpgistAmRoutine = {
    kSpgistAmOid,  &spgistcanreturn, &spgistinsert, &spgistbeginscan, &spgistgettuple,
    &spgistrescan, &spgistendscan,   &spgistbuild,  &spgistgetbitmap,
};

}  // namespace

const IndexAmRoutine* LookupAmRoutine(Oid amoid) {
    switch (amoid) {
        case kBTreeAmOid:
            return &kBtreeAmRoutine;
        case kHashAmOid:
            return &kHashAmRoutine;
        case kGinAmOid:
            return &kGinAmRoutine;
        case kGistAmOid:
            return &kGistAmRoutine;
        case kBrinAmOid:
            return &kBrinAmRoutine;
        case kSpgistAmOid:
            return &kSpgistAmRoutine;
        default:
            return nullptr;
    }
}

// --- Generic index entry points ---

Relation index_open(Oid relid) {
    return RelationOpen(relid);
}

void index_close(Relation rel) {
    if (rel == nullptr)
        return;
    RelationClose(rel);
}

bool index_insert(Relation index, BTKeyKind kind, const void* key, uint16_t key_len,
                  const ItemPointerData& tid) {
    if (index == nullptr || index->rd_rel == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "index_insert: index relation is not open");
    }
    const IndexAmRoutine* routine = LookupAmRoutine(index->rd_rel->relam);
    if (routine == nullptr || routine->aminsert == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "index_insert: AM OID " +
                                                    std::to_string(index->rd_rel->relam) +
                                                    " does not support insert");
    }
    return routine->aminsert(index, kind, key, key_len, tid);
}

BTScanDesc index_beginscan(Relation index, BTKeyKind kind, const BTScanKeyData* scan_key) {
    if (index == nullptr || index->rd_rel == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "index_beginscan: index relation is not open");
    }
    const IndexAmRoutine* routine = LookupAmRoutine(index->rd_rel->relam);
    if (routine == nullptr || routine->ambeginscan == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "index_beginscan: AM OID " +
                                                    std::to_string(index->rd_rel->relam) +
                                                    " does not support scan");
    }
    return routine->ambeginscan(index, kind, scan_key);
}

bool index_getnext_tid(BTScanDesc scan) {
    if (scan == nullptr || scan->index == nullptr || scan->index->rd_rel == nullptr) {
        return false;
    }
    const IndexAmRoutine* routine = LookupAmRoutine(scan->index->rd_rel->relam);
    if (routine == nullptr || routine->amgettuple == nullptr) {
        return false;
    }
    return routine->amgettuple(scan);
}

void index_rescan(BTScanDesc scan, const BTScanKeyData* new_scan_key) {
    if (scan == nullptr || scan->index == nullptr || scan->index->rd_rel == nullptr) {
        return;
    }
    const IndexAmRoutine* routine = LookupAmRoutine(scan->index->rd_rel->relam);
    if (routine == nullptr || routine->amrescan == nullptr) {
        return;
    }
    routine->amrescan(scan, new_scan_key);
}

void index_endscan(BTScanDesc scan) {
    if (scan == nullptr || scan->index == nullptr || scan->index->rd_rel == nullptr) {
        return;
    }
    const IndexAmRoutine* routine = LookupAmRoutine(scan->index->rd_rel->relam);
    if (routine == nullptr || routine->amendscan == nullptr) {
        return;
    }
    routine->amendscan(scan);
}

void index_build(Relation index, BTKeyKind key_kind) {
    if (index == nullptr || index->rd_rel == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "index_build: index relation is not open");
    }
    const IndexAmRoutine* routine = LookupAmRoutine(index->rd_rel->relam);
    if (routine == nullptr || routine->ambuild == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "index_build: AM OID " +
                                                    std::to_string(index->rd_rel->relam) +
                                                    " does not support build");
    }
    routine->ambuild(index, key_kind);
}

bool index_can_return(Relation index) {
    if (index == nullptr || index->rd_rel == nullptr) {
        return false;
    }
    const IndexAmRoutine* routine = LookupAmRoutine(index->rd_rel->relam);
    if (routine == nullptr || routine->amcanreturn == nullptr) {
        return false;
    }
    return routine->amcanreturn(index);
}

int64_t index_getbitmap(BTScanDesc scan, std::vector<ItemPointerData>* tids) {
    if (scan == nullptr || scan->index == nullptr || scan->index->rd_rel == nullptr) {
        return 0;
    }
    const IndexAmRoutine* routine = LookupAmRoutine(scan->index->rd_rel->relam);
    if (routine == nullptr || routine->amgetbitmap == nullptr) {
        return 0;
    }
    return routine->amgetbitmap(scan, tids);
}

}  // namespace pgcpp::access

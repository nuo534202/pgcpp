// indexam.h — Generic index access method API.
//
// Converted from PostgreSQL 15's src/include/access/indexam.h.
//
// Provides the IndexAmRoutine function-pointer table (the AM abstraction),
// LookupAmRoutine (resolves an AM OID to its routine), and the generic
// index_open/close/insert/beginscan/getnext_tid/rescan/endscan/build entry
// points that dispatch to the AM's routines.
//
// pgcpp simplification: the routine table is typed to the btree scan
// descriptor (BTScanDesc) and key kind (BTKeyKind) rather than PostgreSQL's
// generic IndexScanDesc, since btree is the only AM implemented. This keeps
// the dispatch trivial and avoids a second abstraction layer.
#pragma once

#include <cstdint>
#include <vector>

#include "pgcpp/access/nbtree.hpp"  // BTScanDesc, BTScanKeyData, BTKeyKind
#include "pgcpp/access/rel.hpp"
#include "pgcpp/catalog/catalog.hpp"
#include "pgcpp/transaction/heap_tuple.hpp"

namespace pgcpp::access {

// Access-method OIDs (PostgreSQL constants).
constexpr pgcpp::catalog::Oid kBTreeAmOid = 403;    // BTREE_AM_OID
constexpr pgcpp::catalog::Oid kHashAmOid = 405;     // HASH_AM_OID
constexpr pgcpp::catalog::Oid kGistAmOid = 783;     // GIST_AM_OID
constexpr pgcpp::catalog::Oid kGinAmOid = 2742;     // GIN_AM_OID
constexpr pgcpp::catalog::Oid kBrinAmOid = 3580;    // BRIN_AM_OID
constexpr pgcpp::catalog::Oid kSpgistAmOid = 4000;  // SPGIST_AM_OID

// IndexAmRoutine — the function-pointer table for an index access method.
//
// Each field mirrors a PostgreSQL am-... callback. The table is looked up by
// AM OID (LookupAmRoutine) and dispatched to by the generic index_* entry
// points. nullptr fields mean the operation is not supported by the AM.
struct IndexAmRoutine {
    pgcpp::catalog::Oid amoid = pgcpp::catalog::kInvalidOid;

    // amcanreturn — can the AM return heap tuples (for index-only scans)?
    bool (*amcanreturn)(Relation index) = nullptr;

    // aminsert — insert a (key, tid) entry.
    bool (*aminsert)(Relation index, BTKeyKind kind, const void* key, uint16_t key_len,
                     const pgcpp::transaction::ItemPointerData& tid) = nullptr;

    // ambeginscan — start a scan.
    BTScanDesc (*ambeginscan)(Relation index, BTKeyKind kind,
                              const BTScanKeyData* scan_key) = nullptr;

    // amgettuple — fetch the next matching tid.
    bool (*amgettuple)(BTScanDesc scan) = nullptr;

    // amrescan — restart the scan.
    void (*amrescan)(BTScanDesc scan, const BTScanKeyData* new_scan_key) = nullptr;

    // amendscan — release scan resources.
    void (*amendscan)(BTScanDesc scan) = nullptr;

    // ambuild — build a new (empty) index.
    void (*ambuild)(Relation index, BTKeyKind key_kind) = nullptr;

    // amgetbitmap — fetch all matching tids into a bitmap (vector).
    // Returns the number of tids collected.
    int64_t (*amgetbitmap)(BTScanDesc scan,
                           std::vector<pgcpp::transaction::ItemPointerData>* tids) = nullptr;
};

// LookupAmRoutine — resolve an AM OID to its IndexAmRoutine.
//
// Returns a pointer to a static routine table, or nullptr if the AM OID is
// unknown. pgcpp only implements btree (kBTreeAmOid = 403).
const IndexAmRoutine* LookupAmRoutine(pgcpp::catalog::Oid amoid);

// --- Generic index entry points (dispatch to the AM routine) ---

// index_open — open an index relation by OID (PG alias for RelationOpen).
Relation index_open(pgcpp::catalog::Oid relid);

// index_close — close an index relation (decrement refcount).
void index_close(Relation rel);

// index_insert — insert a (key, tid) entry via the AM's aminsert.
bool index_insert(Relation index, BTKeyKind kind, const void* key, uint16_t key_len,
                  const pgcpp::transaction::ItemPointerData& tid);

// index_beginscan — start a scan via the AM's ambeginscan.
BTScanDesc index_beginscan(Relation index, BTKeyKind kind, const BTScanKeyData* scan_key);

// index_getnext_tid — fetch the next matching tid via the AM's amgettuple.
bool index_getnext_tid(BTScanDesc scan);

// index_rescan — restart a scan via the AM's amrescan.
void index_rescan(BTScanDesc scan, const BTScanKeyData* new_scan_key);

// index_endscan — release scan resources via the AM's amendscan.
void index_endscan(BTScanDesc scan);

// index_build — build a new (empty) index via the AM's ambuild.
void index_build(Relation index, BTKeyKind key_kind);

// index_can_return — can the AM return heap tuples? (PG amcanreturn).
bool index_can_return(Relation index);

// index_getbitmap — fetch all matching tids into a vector via the AM's
// amgetbitmap. Returns the number of tids collected.
int64_t index_getbitmap(BTScanDesc scan, std::vector<pgcpp::transaction::ItemPointerData>* tids);

}  // namespace pgcpp::access

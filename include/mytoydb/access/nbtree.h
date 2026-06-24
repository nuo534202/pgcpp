// nbtree.h — B-tree index access method API.
//
// Converted from PostgreSQL 15's src/include/access/nbtree.h.
//
// The B-tree access method provides ordered storage for fast point lookups
// and range scans. Key operations:
//   btinsert    — insert a (key, tid) entry into the index
//   btbeginscan — start a scan (optionally with a search key)
//   btgettuple  — fetch the next matching tid in ascending key order
//   btrescan    — restart the scan from the beginning
//   btendscan   — release scan resources
//
// MyToyDB implements a simplified B-tree:
//   - Supports int32, int64, and text keys
//   - Root starts as a leaf; splits into two leaves with a new internal
//     root when full
//   - Leaf pages are linked via btpo_next for forward scanning
//   - No VACUUM, no page deletion, no deduplication
//
// The API preserves PostgreSQL's function names for compatibility.
#pragma once

#include <cstdint>

#include "mytoydb/access/nbtpage.h"
#include "mytoydb/access/rel.h"
#include "mytoydb/storage/bufmgr.h"
#include "mytoydb/transaction/heap_tuple.h"

namespace mytoydb::access {

// B-tree scan strategy (maps to PostgreSQL's StrategyNumber).
enum class BTStrategy {
    kAll,  // no key filter (full scan)
    kEqual,  // key == scan_key
    kLess,   // key < scan_key
    kLessEqual,  // key <= scan_key
    kGreater,    // key > scan_key
    kGreaterEqual,  // key >= scan_key
};

// BTScanKeyData — the search key for a B-tree scan.
struct BTScanKeyData {
    BTKeyKind kind = BTKeyKind::kInt32;
    const void* key = nullptr;  // pointer to key data (not owned)
    uint16_t key_len = 0;
    BTStrategy strategy = BTStrategy::kAll;
};

// BTScanDescData — descriptor for a B-tree scan.
struct BTScanDescData {
    Relation index = nullptr;          // the index relation
    BTKeyKind key_kind = BTKeyKind::kInt32;  // key type for this index
    BTScanKeyData scan_key;            // search key (strategy + value)

    // Current scan position.
    mytoydb::storage::BlockNumber curr_block = 0;  // current leaf block
    mytoydb::storage::Buffer curr_buf = mytoydb::storage::kInvalidBuffer;
    mytoydb::storage::OffsetNumber curr_offset = 0;  // next offset to check
    bool inited = false;  // true if scan has been positioned

    // The current tuple being returned (tid of the matching heap tuple).
    mytoydb::transaction::ItemPointerData curr_tid;
};

// BTScanDesc — pointer to a BTScanDescData.
using BTScanDesc = BTScanDescData*;

// --- B-tree index operations ---

// btinsert — insert a (key, tid) entry into the B-tree index.
//
// Parameters:
//   index    — the index relation (must have been created with the right key kind)
//   kind     — the key type (int32, int64, text)
//   key      — pointer to the key data
//   key_len  — length of the key data (for text; ignored for fixed-size types)
//   tid      — the heap tuple ID to associate with this key
//
// Returns true on success.
bool btinsert(Relation index, BTKeyKind kind, const void* key,
              uint16_t key_len,
              const mytoydb::transaction::ItemPointerData& tid);

// btbeginscan — start a B-tree scan.
//
// If scan_key is provided, the scan returns only entries matching the
// strategy. If scan_key is nullptr or strategy is kAll, all entries are
// returned in ascending key order.
BTScanDesc btbeginscan(Relation index, BTKeyKind kind,
                        const BTScanKeyData* scan_key);

// btgettuple — fetch the next matching tid from the scan.
//
// Returns true if a matching entry was found (curr_tid is set).
// Returns false if the scan is complete.
bool btgettuple(BTScanDesc scan);

// btrescan — restart the scan from the beginning.
// Optionally update the scan key.
void btrescan(BTScanDesc scan, const BTScanKeyData* new_scan_key);

// btendscan — release scan resources.
void btendscan(BTScanDesc scan);

// --- Index creation ---

// btbuild — create a new B-tree index.
// Initializes the meta page (block 0) and an empty root leaf page (block 1).
// The index relation must already have storage created.
void btbuild(Relation index, BTKeyKind key_kind);

// --- Helpers (exposed for testing) ---

// _bt_search_leaf — find the leaf page and offset for a given key.
// Returns the leaf buffer (pinned) and sets *offset to the first entry
// >= key (or one past the last entry if all entries are < key).
mytoydb::storage::Buffer _bt_search_leaf(Relation index, BTKeyKind kind,
                                          const void* key, uint16_t key_len,
                                          mytoydb::storage::OffsetNumber* offset);

// _bt_find_insert_pos — find the position to insert a key in a leaf page.
// Returns the offset number (1-based) where the key should be inserted.
mytoydb::storage::OffsetNumber _bt_find_insert_pos(
    mytoydb::storage::Page page, BTKeyKind kind,
    const void* key, uint16_t key_len);

// _bt_find_scan_pos — find the first entry >= key in a leaf page.
// Returns the offset number, or one past the last entry if not found.
mytoydb::storage::OffsetNumber _bt_find_scan_pos(
    mytoydb::storage::Page page, BTKeyKind kind,
    const void* key, uint16_t key_len);

}  // namespace mytoydb::access

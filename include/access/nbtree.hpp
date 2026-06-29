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
// pgcpp implements a simplified B-tree:
//   - Supports int32, int64, and text keys
//   - Root starts as a leaf; splits into two leaves with a new internal
//     root when full
//   - Leaf pages are linked via btpo_next for forward scanning
//   - No VACUUM, no page deletion, no deduplication
//
// The API preserves PostgreSQL's function names for compatibility.
#pragma once

#include <cstdint>
#include <vector>

#include "access/nbtpage.hpp"
#include "access/rel.hpp"
#include "storage/bufmgr.hpp"
#include "transaction/heap_tuple.hpp"

namespace pgcpp::access {

// B-tree scan strategy (maps to PostgreSQL's StrategyNumber).
enum class BTStrategy {
    kAll,           // no key filter (full scan)
    kEqual,         // key == scan_key
    kLess,          // key < scan_key
    kLessEqual,     // key <= scan_key
    kGreater,       // key > scan_key
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
    Relation index = nullptr;                // the index relation
    BTKeyKind key_kind = BTKeyKind::kInt32;  // key type for this index
    BTScanKeyData scan_key;                  // search key (strategy + value)

    // Current scan position.
    pgcpp::storage::BlockNumber curr_block = 0;  // current leaf block
    pgcpp::storage::Buffer curr_buf = pgcpp::storage::kInvalidBuffer;
    pgcpp::storage::OffsetNumber curr_offset = 0;  // next offset to check
    bool inited = false;                           // true if scan has been positioned

    // The current tuple being returned (tid of the matching heap tuple).
    pgcpp::transaction::ItemPointerData curr_tid;
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
bool btinsert(Relation index, BTKeyKind kind, const void* key, uint16_t key_len,
              const pgcpp::transaction::ItemPointerData& tid);

// btbeginscan — start a B-tree scan.
//
// If scan_key is provided, the scan returns only entries matching the
// strategy. If scan_key is nullptr or strategy is kAll, all entries are
// returned in ascending key order.
BTScanDesc btbeginscan(Relation index, BTKeyKind kind, const BTScanKeyData* scan_key);

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

// --- P0 extensions (Task 15.8.5 / GAP-M8-F05/F06) ---

// btcanreturn — can the B-tree AM return heap tuples (index-only scans)?
// Always returns true for B-tree (PostgreSQL's amcanreturn).
bool btcanreturn(Relation index);

// btgetbitmap — fetch all matching tids into a bitmap (vector).
// Repeatedly calls btgettuple until the scan is exhausted, appending each
// matching tid to `tids`. Returns the number of tids collected.
// The scan must have been started with btbeginscan; this function does not
// reset the scan position, so callers typically begin a fresh scan first.
int64_t btgetbitmap(BTScanDesc scan, std::vector<pgcpp::transaction::ItemPointerData>* tids);

// --- Index creation ---

// btbuild — create a new B-tree index.
// Initializes the meta page (block 0) and an empty root leaf page (block 1).
// The index relation must already have storage created.
void btbuild(Relation index, BTKeyKind key_kind);

// --- Helpers (exposed for testing) ---

// _bt_search_leaf — find the leaf page and offset for a given key.
// Returns the leaf buffer (pinned) and sets *offset to the first entry
// >= key (or one past the last entry if all entries are < key).
pgcpp::storage::Buffer _bt_search_leaf(Relation index, BTKeyKind kind, const void* key,
                                       uint16_t key_len, pgcpp::storage::OffsetNumber* offset);

// _bt_find_insert_pos — find the position to insert a key in a leaf page.
// Returns the offset number (1-based) where the key should be inserted.
pgcpp::storage::OffsetNumber _bt_find_insert_pos(pgcpp::storage::Page page, BTKeyKind kind,
                                                 const void* key, uint16_t key_len);

// _bt_find_scan_pos — find the first entry >= key in a leaf page.
// Returns the offset number, or one past the last entry if not found.
pgcpp::storage::OffsetNumber _bt_find_scan_pos(pgcpp::storage::Page page, BTKeyKind kind,
                                               const void* key, uint16_t key_len);

// --- P0 helper extensions (Task 15.8.5 / FN15/FN23/FN28) ---

// _bt_binsrch — binary search a B-tree page for a key.
//
// When for_insert is false, returns the first offset whose item key is >= the
// search key (i.e., the scan start position). When for_insert is true, returns
// the first offset whose item key is strictly > the search key (used to place
// new keys after all equal keys during insertion). Returns an offset in
// [1, max_offset+1]; max_offset+1 means the key belongs past the last item.
//
// Assumes all line pointers on the page are normal (true for compacted
// B-tree pages in pgcpp; the linear _bt_find_insert_pos / _bt_find_scan_pos
// helpers remain available for pages that may contain dead entries).
pgcpp::storage::OffsetNumber _bt_binsrch(pgcpp::storage::Page page, BTKeyKind kind, const void* key,
                                         uint16_t key_len, bool for_insert);

// _bt_getbuf — read and pin a B-tree page by block number.
pgcpp::storage::Buffer _bt_getbuf(Relation index, pgcpp::storage::BlockNumber blkno);

// _bt_relandgetbuf — release the current buffer and read a new one.
// Equivalent to ReleaseBuffer(buf); _bt_getbuf(index, blkno).
pgcpp::storage::Buffer _bt_relandgetbuf(Relation index, pgcpp::storage::Buffer buf,
                                        pgcpp::storage::BlockNumber blkno);

// _bt_relbuf — release a pinned B-tree buffer.
void _bt_relbuf(Relation index, pgcpp::storage::Buffer buf);

}  // namespace pgcpp::access

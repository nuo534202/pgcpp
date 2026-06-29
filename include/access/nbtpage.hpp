// nbtpage.h — B-tree page layout and operations.
//
// Converted from PostgreSQL 15's src/include/access/nbtree.h.
//
// B-tree pages use the standard page layout (header + line pointers + items
// + special area). The special area at the end of each page holds
// BTPageOpaqueData, which records the page type (leaf/internal/root),
// sibling links for sequential scanning, and the tree level.
//
// Page types:
//   Meta page (block 0)  — holds the root block number and magic number
//   Root page             — the top of the B-tree (may be leaf or internal)
//   Internal page         — contains (key, child) entries for navigation
//   Leaf page             — contains (key, tid) entries pointing to heap tuples
//
// Leaf pages are linked in ascending key order via btpo_next, enabling
// efficient forward range scans.
//
// Index entry format (leaf items):
//   [ItemPointerData tid (6 bytes)] [key data (variable)]
//
// Key data is stored inline. For by-value types (int4, int8), the key is
// stored as a fixed-size value. For text, it's a length-prefixed string.
#pragma once

#include <cstdint>

#include "storage/block.hpp"
#include "storage/bufpage.hpp"
#include "transaction/heap_tuple.hpp"

namespace pgcpp::access {

// B-tree page flags.
constexpr uint32_t kBtpLeaf = 0x01;     // leaf page
constexpr uint32_t kBtpRoot = 0x02;     // root page
constexpr uint32_t kBtpMeta = 0x04;     // meta page
constexpr uint32_t kBtpDeleted = 0x08;  // page has been deleted

// Size of the B-tree special area (BTPageOpaqueData).
constexpr int kBTPageOpaqueSize = 16;

// BTPageOpaqueData — B-tree page metadata stored in the page's special area.
struct BTPageOpaqueData {
    pgcpp::storage::BlockNumber btpo_prev = 0;  // previous leaf (backward scan)
    pgcpp::storage::BlockNumber btpo_next = 0;  // next leaf (forward scan)
    uint32_t btpo_flags = 0;                    // page flags (kBtpLeaf, etc.)
    uint32_t btpo_level = 0;                    // 0 = leaf, 1+ = internal
};

using BTPageOpaque = BTPageOpaqueData*;

// BTItemData — a B-tree index entry on a leaf page.
// The key data follows immediately after the tid.
struct BTItemData {
    pgcpp::transaction::ItemPointerData tid;  // heap tuple ID (6 bytes)
    // key data follows (variable length)
};

using BTItem = BTItemData*;

// B-tree meta page data (stored on block 0).
struct BTMetaPageData {
    uint32_t magic = 0;  // magic number for sanity check
    uint32_t version = 0;
    pgcpp::storage::BlockNumber root = 0;      // root block number
    uint32_t level = 0;                        // root level
    pgcpp::storage::BlockNumber fastroot = 0;  // fast path root
};

constexpr uint32_t kBtreeMagic = 0x0530;

// --- Key type identifiers ---
//
// pgcpp supports int4, int8, and text keys for B-tree indexes.
// The key type is stored in the index relation's pg_class entry (relam)
// and passed to comparison functions.

enum class BTKeyKind : uint32_t {
    kInt32 = 0,  // int4 key (4 bytes)
    kInt64 = 1,  // int8 key (8 bytes)
    kText = 2,   // text key (variable length, null-terminated)
};

// --- Page access functions ---

// _bt_page_getopaque — return the opaque data pointer for a B-tree page.
// The opaque data is stored at the beginning of the page's special area.
inline BTPageOpaque _bt_page_getopaque(pgcpp::storage::Page page) {
    auto* phdr = reinterpret_cast<pgcpp::storage::PageHeader>(page);
    return reinterpret_cast<BTPageOpaque>(page + phdr->pd_special);
}

// _bt_init_page — initialize a B-tree page with the given flags.
// Sets up the page header, line pointer array, and opaque data.
void _bt_init_page(pgcpp::storage::Page page, uint32_t flags, uint32_t level);

// _bt_is_leaf_page — true if the page is a leaf.
inline bool _bt_is_leaf_page(pgcpp::storage::Page page) {
    return (_bt_page_getopaque(page)->btpo_flags & kBtpLeaf) != 0;
}

// _bt_is_root_page — true if the page is the root.
inline bool _bt_is_root_page(pgcpp::storage::Page page) {
    return (_bt_page_getopaque(page)->btpo_flags & kBtpRoot) != 0;
}

// --- Key comparison ---

// _bt_compare_keys — compare two keys of the given kind.
// Returns < 0 if key1 < key2, 0 if equal, > 0 if key1 > key2.
int _bt_compare_keys(BTKeyKind kind, const void* key1, uint16_t len1, const void* key2,
                     uint16_t len2);

// _bt_compare_int32 — compare two int32 keys.
inline int _bt_compare_int32(int32_t a, int32_t b) {
    if (a < b)
        return -1;
    if (a > b)
        return 1;
    return 0;
}

// _bt_compare_int64 — compare two int64 keys.
inline int _bt_compare_int64(int64_t a, int64_t b) {
    if (a < b)
        return -1;
    if (a > b)
        return 1;
    return 0;
}

// _bt_compare_text — compare two text keys (as C strings).
int _bt_compare_text(const char* a, uint16_t len_a, const char* b, uint16_t len_b);

// --- Index entry helpers ---

// _bt_item_get_key — return a pointer to the key data in a BTItem.
inline const void* _bt_item_get_key(BTItem item) {
    return reinterpret_cast<const char*>(item) + sizeof(BTItemData);
}

// _bt_item_get_key_len — compute the key length from the item's total size.
inline uint16_t _bt_item_get_key_len(uint16_t item_size) {
    return item_size - sizeof(BTItemData);
}

// Compute the size of an index entry for a given key.
uint16_t _bt_item_size(BTKeyKind kind, const void* key, uint16_t key_len);

// Build an index entry in the provided buffer.
// Returns the total entry size.
uint16_t _bt_build_item(BTItem item, BTKeyKind kind, const void* key, uint16_t key_len,
                        const pgcpp::transaction::ItemPointerData& tid);

// --- Meta page operations ---

// _bt_init_meta_page — initialize the meta page (block 0).
void _bt_init_meta_page(pgcpp::storage::Page page, pgcpp::storage::BlockNumber root_block);

// _bt_get_meta — read the meta page data.
BTMetaPageData _bt_get_meta(pgcpp::storage::Page page);

// _bt_update_meta — update the root block in the meta page.
void _bt_update_meta(pgcpp::storage::Page page, pgcpp::storage::BlockNumber root_block);

}  // namespace pgcpp::access

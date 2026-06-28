// nbtpage.cpp — B-tree page operations implementation.
//
// Converted from PostgreSQL 15's src/backend/access/nbtree/nbtpage.c.
//
// Provides page initialization, key comparison, and index entry construction
// for B-tree pages. The B-tree uses the standard page layout with a
// BTPageOpaqueData special area at the end of each page.

#include "pgcpp/access/nbtpage.hpp"

#include <cstring>

#include "pgcpp/storage/bufpage.hpp"

namespace mytoydb::access {

using mytoydb::storage::kBlckSz;
using mytoydb::storage::Page;
using mytoydb::storage::PageHeader;
using mytoydb::storage::PageInit;
using mytoydb::transaction::ItemPointerData;

// --- Page initialization ---

void _bt_init_page(Page page, uint32_t flags, uint32_t level) {
    // Initialize the page with a special area for BTPageOpaqueData.
    PageInit(page, kBlckSz, sizeof(BTPageOpaqueData));

    // Set the opaque data.
    BTPageOpaque opaque = _bt_page_getopaque(page);
    opaque->btpo_prev = 0;
    opaque->btpo_next = 0;
    opaque->btpo_flags = flags;
    opaque->btpo_level = level;
}

// --- Key comparison ---

int _bt_compare_text(const char* a, uint16_t len_a, const char* b, uint16_t len_b) {
    uint16_t min_len = len_a < len_b ? len_a : len_b;
    int cmp = std::memcmp(a, b, min_len);
    if (cmp != 0)
        return cmp;
    // If one is a prefix of the other, the shorter one is smaller.
    if (len_a < len_b)
        return -1;
    if (len_a > len_b)
        return 1;
    return 0;
}

int _bt_compare_keys(BTKeyKind kind, const void* key1, uint16_t len1, const void* key2,
                     uint16_t len2) {
    switch (kind) {
        case BTKeyKind::kInt32: {
            int32_t a, b;
            std::memcpy(&a, key1, sizeof(int32_t));
            std::memcpy(&b, key2, sizeof(int32_t));
            return _bt_compare_int32(a, b);
        }
        case BTKeyKind::kInt64: {
            int64_t a, b;
            std::memcpy(&a, key1, sizeof(int64_t));
            std::memcpy(&b, key2, sizeof(int64_t));
            return _bt_compare_int64(a, b);
        }
        case BTKeyKind::kText:
            return _bt_compare_text(static_cast<const char*>(key1), len1,
                                    static_cast<const char*>(key2), len2);
    }
    return 0;
}

// --- Index entry construction ---

uint16_t _bt_item_size(BTKeyKind kind, const void* key, uint16_t key_len) {
    switch (kind) {
        case BTKeyKind::kInt32:
            return static_cast<uint16_t>(sizeof(BTItemData) + sizeof(int32_t));
        case BTKeyKind::kInt64:
            return static_cast<uint16_t>(sizeof(BTItemData) + sizeof(int64_t));
        case BTKeyKind::kText:
            return static_cast<uint16_t>(sizeof(BTItemData) + key_len);
    }
    return static_cast<uint16_t>(sizeof(BTItemData));
}

uint16_t _bt_build_item(BTItem item, BTKeyKind kind, const void* key, uint16_t key_len,
                        const ItemPointerData& tid) {
    item->tid = tid;
    char* key_ptr = reinterpret_cast<char*>(item) + sizeof(BTItemData);

    switch (kind) {
        case BTKeyKind::kInt32:
            std::memcpy(key_ptr, key, sizeof(int32_t));
            return static_cast<uint16_t>(sizeof(BTItemData) + sizeof(int32_t));
        case BTKeyKind::kInt64:
            std::memcpy(key_ptr, key, sizeof(int64_t));
            return static_cast<uint16_t>(sizeof(BTItemData) + sizeof(int64_t));
        case BTKeyKind::kText:
            std::memcpy(key_ptr, key, key_len);
            return static_cast<uint16_t>(sizeof(BTItemData) + key_len);
    }
    return static_cast<uint16_t>(sizeof(BTItemData));
}

// --- Meta page operations ---

void _bt_init_meta_page(Page page, mytoydb::storage::BlockNumber root_block) {
    // Meta page has no special area (it stores meta data in the page body).
    PageInit(page, kBlckSz, 0);

    auto* meta = reinterpret_cast<BTMetaPageData*>(page + sizeof(mytoydb::storage::PageHeaderData));
    meta->magic = kBtreeMagic;
    meta->version = 1;
    meta->root = root_block;
    meta->level = 0;
    meta->fastroot = root_block;
}

BTMetaPageData _bt_get_meta(Page page) {
    auto* meta = reinterpret_cast<BTMetaPageData*>(page + sizeof(mytoydb::storage::PageHeaderData));
    return *meta;
}

void _bt_update_meta(Page page, mytoydb::storage::BlockNumber root_block) {
    auto* meta = reinterpret_cast<BTMetaPageData*>(page + sizeof(mytoydb::storage::PageHeaderData));
    meta->root = root_block;
    meta->fastroot = root_block;
}

}  // namespace mytoydb::access

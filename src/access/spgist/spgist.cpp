// spgist.cpp — SP-GiST (Space-Partitioned GiST) access method implementation.
//
// Converted from PostgreSQL 15's src/backend/access/spgst/spgist.c.
//
// SP-GiST supports space-partitioned trees. For int32 keys, pgcpp implements
// a binary radix trie: each internal node tests one bit of the key and has
// up to two children (one for bit=0, one for bit=1). Leaf nodes hold
// (key, tid) entries.
//
// pgcpp simplifications:
//   - Supports int32 keys with binary radix partitioning
//   - Fixed 32-bit depth (one bit per level, levels 0..31)
//   - No VACUUM, no pickout, no compression, no prefix tuples
//   - Leaf split creates a new internal node at the highest differing bit
//
// Page layout:
//   Block 0: meta page (SpgistMetaPageData in content area)
//   Block 1: root page (starts as leaf)
//   Leaf pages: items are BTItemData (tid + int32 key)
//   Internal pages: items are SpgistInternalEntry (bit_value + child block)
//     btpo_level stores the bit position being tested (0 = LSB .. 31 = MSB)

#include "access/spgist.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#include "access/nbtpage.hpp"
#include "access/rel.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/memory_context.hpp"
#include "storage/bufmgr.hpp"
#include "storage/bufpage.hpp"
#include "storage/smgr.hpp"
#include "transaction/heap_tuple.hpp"

namespace pgcpp::access {
using pgcpp::nodes::destroyPallocNode;
using pgcpp::nodes::makePallocNode;

namespace {

using pgcpp::storage::BlockNumber;
using pgcpp::storage::Buffer;
using pgcpp::storage::BufferGetPage;
using pgcpp::storage::ForkNumber;
using pgcpp::storage::Item;
using pgcpp::storage::ItemIdGetLength;
using pgcpp::storage::ItemIdIsNormal;
using pgcpp::storage::kBlckSz;
using pgcpp::storage::kInvalidBuffer;
using pgcpp::storage::kInvalidOffsetNumber;
using pgcpp::storage::kPageHeaderSize;
using pgcpp::storage::MarkBufferDirty;
using pgcpp::storage::OffsetNumber;
using pgcpp::storage::Page;
using pgcpp::storage::PageAddItem;
using pgcpp::storage::PageGetItem;
using pgcpp::storage::PageGetItemId;
using pgcpp::storage::PageGetMaxOffsetNumber;
using pgcpp::storage::PageInit;
using pgcpp::storage::ReadBuffer;
using pgcpp::storage::ReadBufferMode;
using pgcpp::storage::ReleaseBuffer;
using pgcpp::transaction::ItemPointerData;

// SP-GiST page flags (stored in BTPageOpaqueData::btpo_flags).
constexpr uint32_t kSpgistMeta = 0x0100;      // meta page
constexpr uint32_t kSpgistLeaf = 0x0200;      // leaf page
constexpr uint32_t kSpgistInternal = 0x0400;  // internal page
constexpr uint32_t kSpgistRoot = 0x0800;      // root page

// SP-GiST meta page magic.
constexpr uint32_t kSpgistMagic = 0x0685;

// SP-GiST meta page data (stored in the content area of block 0).
struct SpgistMetaPageData {
    uint32_t magic = 0;
    BlockNumber root = 0;
};

// SP-GiST internal node entry: one per child (at most 2: bit=0 and bit=1).
struct SpgistInternalEntry {
    uint32_t bit_value = 0;  // 0 or 1 — the bit value this child represents
    BlockNumber child = 0;   // child block number
};

// Per-scan opaque state.
struct SpgistScanOpaque {
    Buffer currbuf = kInvalidBuffer;  // current leaf buffer being scanned
    OffsetNumber curroff = 0;         // next offset to check
    bool inited = false;
    BlockNumber scan_block = 0;  // current block for full-scan walk
};

// --- Meta page helpers ---

void spgist_init_meta_page(Page page, BlockNumber root) {
    PageInit(page, kBlckSz, kBTPageOpaqueSize);
    auto* opaque = _bt_page_getopaque(page);
    opaque->btpo_flags = kSpgistMeta;
    auto* meta = reinterpret_cast<SpgistMetaPageData*>(page + kPageHeaderSize);
    meta->magic = kSpgistMagic;
    meta->root = root;
}

SpgistMetaPageData spgist_get_meta(Page page) {
    SpgistMetaPageData meta;
    std::memcpy(&meta, page + kPageHeaderSize, sizeof(meta));
    return meta;
}

// --- Page creation ---

Buffer spgist_create_page(Relation index, BlockNumber block_num, uint32_t flags, uint32_t level) {
    char pagebuf[kBlckSz];
    PageInit(pagebuf, kBlckSz, kBTPageOpaqueSize);
    auto* opaque = _bt_page_getopaque(pagebuf);
    opaque->btpo_flags = flags;
    opaque->btpo_prev = 0;
    opaque->btpo_next = 0;
    opaque->btpo_level = level;
    index->rd_smgr = RelationGetSmgr(index);
    pgcpp::storage::smgrextend(index->rd_smgr, ForkNumber::kMain, block_num, pagebuf, false);
    return ReadBuffer(index->rd_smgr, ForkNumber::kMain, block_num, ReadBufferMode::kNormal);
}

// Extract bit `pos` (0=LSB..31=MSB) from a 32-bit key.
uint32_t extract_bit(int32_t key, uint32_t pos) {
    return (static_cast<uint32_t>(key) >> pos) & 1u;
}

// Read a leaf entry's int32 key.
int32_t spgist_leaf_get_key(BTItem item) {
    int32_t key;
    std::memcpy(&key, _bt_item_get_key(item), sizeof(key));
    return key;
}

// Check if a leaf entry matches the scan key.
bool spgist_entry_matches(int32_t entry_key, const BTScanKeyData& scan_key) {
    if (scan_key.strategy == BTStrategy::kAll || scan_key.key == nullptr)
        return true;

    int32_t scan_val;
    std::memcpy(&scan_val, scan_key.key, sizeof(scan_val));

    switch (scan_key.strategy) {
        case BTStrategy::kEqual:
            return entry_key == scan_val;
        case BTStrategy::kLess:
            return entry_key < scan_val;
        case BTStrategy::kLessEqual:
            return entry_key <= scan_val;
        case BTStrategy::kGreater:
            return entry_key > scan_val;
        case BTStrategy::kGreaterEqual:
            return entry_key >= scan_val;
        case BTStrategy::kAll:
            return true;
    }
    return false;
}

// Find the highest bit position where the keys differ (returns -1 if all equal).
int32_t highest_differing_bit(const std::vector<int32_t>& keys) {
    if (keys.empty())
        return -1;
    uint32_t mask = 0;
    uint32_t first = static_cast<uint32_t>(keys[0]);
    for (size_t i = 1; i < keys.size(); i++) {
        mask |= first ^ static_cast<uint32_t>(keys[i]);
    }
    if (mask == 0)
        return -1;
    // Find the highest set bit.
    int32_t pos = 31;
    while (pos >= 0 && (mask & (1u << pos)) == 0)
        pos--;
    return pos;
}

}  // namespace

// --- Index creation ---

void spgistbuild(Relation index, [[maybe_unused]] BTKeyKind key_kind) {
    index->rd_smgr = RelationGetSmgr(index);

    // Block 0: meta page (root = block 1).
    char metabuf[kBlckSz];
    spgist_init_meta_page(metabuf, 1);
    pgcpp::storage::smgrextend(index->rd_smgr, ForkNumber::kMain, 0, metabuf, false);

    // Block 1: root leaf page.
    char rootbuf[kBlckSz];
    PageInit(rootbuf, kBlckSz, kBTPageOpaqueSize);
    auto* opaque = _bt_page_getopaque(rootbuf);
    opaque->btpo_flags = kSpgistLeaf | kSpgistRoot;
    opaque->btpo_next = 0;
    opaque->btpo_level = 0;
    pgcpp::storage::smgrextend(index->rd_smgr, ForkNumber::kMain, 1, rootbuf, false);
}

// --- Insertion ---

bool spgistinsert(Relation index, BTKeyKind kind, const void* key,
                  [[maybe_unused]] uint16_t key_len, const ItemPointerData& tid) {
    if (kind != BTKeyKind::kInt32) {
        ereport(pgcpp::error::LogLevel::kWarning, "spgistinsert: only int32 keys are supported");
        return false;
    }

    index->rd_smgr = RelationGetSmgr(index);
    int32_t key_val;
    std::memcpy(&key_val, key, sizeof(key_val));

    // Read meta for root.
    Buffer meta_buf = ReadBuffer(index->rd_smgr, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
    Page meta_page = BufferGetPage(meta_buf);
    SpgistMetaPageData meta = spgist_get_meta(meta_page);
    ReleaseBuffer(meta_buf);

    BlockNumber root_block = meta.root;
    Buffer buf = ReadBuffer(index->rd_smgr, ForkNumber::kMain, root_block, ReadBufferMode::kNormal);
    Page page = BufferGetPage(buf);

    // Build the leaf entry (same format as btree: tid + int32 key).
    uint16_t item_size = _bt_item_size(kind, key, key_len);
    char item_buf[256];
    _bt_build_item(reinterpret_cast<BTItem>(item_buf), kind, key, key_len, tid);

    // Descend through internal nodes to the target leaf.
    while (!(_bt_page_getopaque(page)->btpo_flags & kSpgistLeaf)) {
        uint32_t level = _bt_page_getopaque(page)->btpo_level;
        uint32_t bit = extract_bit(key_val, level);

        // Find the child entry with matching bit_value.
        BlockNumber child = 0;
        bool found = false;
        OffsetNumber max_off = PageGetMaxOffsetNumber(page);
        for (OffsetNumber off = 1; off <= max_off; off++) {
            auto* item_id = PageGetItemId(page, off);
            if (!ItemIdIsNormal(item_id))
                continue;
            auto* entry = reinterpret_cast<SpgistInternalEntry*>(PageGetItem(page, item_id));
            if (entry->bit_value == bit) {
                child = entry->child;
                found = true;
                break;
            }
        }

        if (!found) {
            // No child for this bit value — create a new leaf.
            BlockNumber nblocks = RelationGetNumberOfBlocks(index);
            Buffer new_leaf_buf = spgist_create_page(index, nblocks, kSpgistLeaf, 0);
            Page new_leaf_page = BufferGetPage(new_leaf_buf);
            PageAddItem(new_leaf_page, reinterpret_cast<Item>(item_buf), item_size,
                        kInvalidOffsetNumber, false);
            MarkBufferDirty(new_leaf_buf);
            ReleaseBuffer(new_leaf_buf);

            // Add an internal entry pointing to the new leaf.
            SpgistInternalEntry new_entry;
            new_entry.bit_value = bit;
            new_entry.child = nblocks;
            PageAddItem(page, reinterpret_cast<Item>(&new_entry), sizeof(new_entry),
                        kInvalidOffsetNumber, false);
            MarkBufferDirty(buf);
            ReleaseBuffer(buf);
            return true;
        }

        ReleaseBuffer(buf);
        buf = ReadBuffer(index->rd_smgr, ForkNumber::kMain, child, ReadBufferMode::kNormal);
        page = BufferGetPage(buf);
    }

    // We're at a leaf — try to insert.
    OffsetNumber result =
        PageAddItem(page, reinterpret_cast<Item>(item_buf), item_size, kInvalidOffsetNumber, false);
    if (result != kInvalidOffsetNumber) {
        MarkBufferDirty(buf);
        ReleaseBuffer(buf);
        return true;
    }

    // Leaf is full — split it.
    // Collect all existing entries + the new one.
    std::vector<std::vector<char>> entries;
    std::vector<int32_t> keys;
    OffsetNumber max_off = PageGetMaxOffsetNumber(page);
    for (OffsetNumber off = 1; off <= max_off; off++) {
        auto* item_id = PageGetItemId(page, off);
        if (!ItemIdIsNormal(item_id))
            continue;
        uint16_t len = ItemIdGetLength(item_id);
        const char* data = PageGetItem(page, item_id);
        entries.emplace_back(data, data + len);
        int32_t k;
        std::memcpy(&k, _bt_item_get_key(reinterpret_cast<BTItem>(const_cast<char*>(data))),
                    sizeof(k));
        keys.push_back(k);
    }
    entries.emplace_back(item_buf, item_buf + item_size);
    keys.push_back(key_val);

    int32_t split_bit = highest_differing_bit(keys);
    if (split_bit < 0) {
        // All keys are identical — can't split by bit. Drop the new entry.
        ReleaseBuffer(buf);
        ereport(pgcpp::error::LogLevel::kWarning,
                "spgistinsert: cannot split leaf with duplicate keys");
        return false;
    }

    // Create two new leaf pages.
    BlockNumber nblocks = RelationGetNumberOfBlocks(index);
    BlockNumber left_block = nblocks;
    BlockNumber right_block = nblocks + 1;

    Buffer left_buf = spgist_create_page(index, left_block, kSpgistLeaf, 0);
    Buffer right_buf = spgist_create_page(index, right_block, kSpgistLeaf, 0);
    Page left_page = BufferGetPage(left_buf);
    Page right_page = BufferGetPage(right_buf);

    for (size_t i = 0; i < entries.size(); i++) {
        if (extract_bit(keys[i], static_cast<uint32_t>(split_bit)) == 0) {
            PageAddItem(left_page, reinterpret_cast<Item>(const_cast<char*>(entries[i].data())),
                        entries[i].size(), kInvalidOffsetNumber, false);
        } else {
            PageAddItem(right_page, reinterpret_cast<Item>(const_cast<char*>(entries[i].data())),
                        entries[i].size(), kInvalidOffsetNumber, false);
        }
    }

    MarkBufferDirty(left_buf);
    MarkBufferDirty(right_buf);
    ReleaseBuffer(left_buf);
    ReleaseBuffer(right_buf);

    // Convert the current leaf page into an internal node at split_bit.
    PageInit(page, kBlckSz, kBTPageOpaqueSize);
    auto* opaque = _bt_page_getopaque(page);
    bool is_root = (opaque->btpo_flags & kSpgistRoot) != 0;
    opaque->btpo_flags = kSpgistInternal | (is_root ? kSpgistRoot : 0u);
    opaque->btpo_level = static_cast<uint32_t>(split_bit);

    SpgistInternalEntry left_entry;
    left_entry.bit_value = 0;
    left_entry.child = left_block;
    PageAddItem(page, reinterpret_cast<Item>(&left_entry), sizeof(left_entry), kInvalidOffsetNumber,
                false);

    SpgistInternalEntry right_entry;
    right_entry.bit_value = 1;
    right_entry.child = right_block;
    PageAddItem(page, reinterpret_cast<Item>(&right_entry), sizeof(right_entry),
                kInvalidOffsetNumber, false);

    MarkBufferDirty(buf);
    ReleaseBuffer(buf);
    return true;
}

// --- Scan ---

BTScanDesc spgistbeginscan(Relation index, BTKeyKind kind, const BTScanKeyData* scan_key) {
    BTScanDesc scan = makePallocNode<BTScanDescData>();
    scan->index = index;
    scan->key_kind = kind;
    scan->curr_buf = kInvalidBuffer;
    scan->inited = false;

    if (scan_key != nullptr) {
        scan->scan_key = *scan_key;
    } else {
        scan->scan_key.kind = kind;
        scan->scan_key.key = nullptr;
        scan->scan_key.key_len = 0;
        scan->scan_key.strategy = BTStrategy::kAll;
    }

    auto* opaque = makePallocNode<SpgistScanOpaque>();
    scan->opaque = opaque;
    return scan;
}

// Descend the trie to the leaf for an equality scan. Returns the leaf buffer
// (pinned) or kInvalidBuffer if no matching leaf exists.
Buffer spgist_descend(Relation index, int32_t key_val) {
    Buffer meta_buf = ReadBuffer(index->rd_smgr, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
    Page meta_page = BufferGetPage(meta_buf);
    SpgistMetaPageData meta = spgist_get_meta(meta_page);
    ReleaseBuffer(meta_buf);

    Buffer buf = ReadBuffer(index->rd_smgr, ForkNumber::kMain, meta.root, ReadBufferMode::kNormal);
    Page page = BufferGetPage(buf);

    while (!(_bt_page_getopaque(page)->btpo_flags & kSpgistLeaf)) {
        uint32_t level = _bt_page_getopaque(page)->btpo_level;
        uint32_t bit = extract_bit(key_val, level);

        BlockNumber child = 0;
        bool found = false;
        OffsetNumber max_off = PageGetMaxOffsetNumber(page);
        for (OffsetNumber off = 1; off <= max_off; off++) {
            auto* item_id = PageGetItemId(page, off);
            if (!ItemIdIsNormal(item_id))
                continue;
            auto* entry = reinterpret_cast<SpgistInternalEntry*>(PageGetItem(page, item_id));
            if (entry->bit_value == bit) {
                child = entry->child;
                found = true;
                break;
            }
        }

        if (!found) {
            ReleaseBuffer(buf);
            return kInvalidBuffer;
        }

        ReleaseBuffer(buf);
        buf = ReadBuffer(index->rd_smgr, ForkNumber::kMain, child, ReadBufferMode::kNormal);
        page = BufferGetPage(buf);
    }

    return buf;
}

bool spgistgettuple(BTScanDesc scan) {
    Relation index = scan->index;
    index->rd_smgr = RelationGetSmgr(index);
    auto* so = static_cast<SpgistScanOpaque*>(scan->opaque);
    if (so == nullptr)
        return false;

    if (!so->inited) {
        so->inited = true;
        so->currbuf = kInvalidBuffer;
        so->curroff = 0;

        if (scan->scan_key.strategy == BTStrategy::kEqual && scan->scan_key.key != nullptr) {
            // Equality scan: descend to the target leaf.
            int32_t key_val;
            std::memcpy(&key_val, scan->scan_key.key, sizeof(key_val));
            so->currbuf = spgist_descend(index, key_val);
            so->curroff = 1;
        } else {
            // Full / range scan: walk all blocks, visiting leaves.
            so->scan_block = 1;  // start after meta
            so->currbuf = kInvalidBuffer;
        }
    }

    // Equality scan: search the descended leaf.
    if (scan->scan_key.strategy == BTStrategy::kEqual && scan->scan_key.key != nullptr) {
        while (so->currbuf != kInvalidBuffer) {
            Page page = BufferGetPage(so->currbuf);
            OffsetNumber max_off = PageGetMaxOffsetNumber(page);

            while (so->curroff <= max_off) {
                auto* item_id = PageGetItemId(page, so->curroff);
                so->curroff++;

                if (!ItemIdIsNormal(item_id))
                    continue;

                auto* item = reinterpret_cast<BTItem>(PageGetItem(page, item_id));
                int32_t entry_key = spgist_leaf_get_key(item);

                if (spgist_entry_matches(entry_key, scan->scan_key)) {
                    scan->curr_tid = item->tid;
                    return true;
                }
            }

            ReleaseBuffer(so->currbuf);
            so->currbuf = kInvalidBuffer;
        }
        return false;
    }

    // Full / range scan: walk all blocks, visiting leaves.
    BlockNumber nblocks = RelationGetNumberOfBlocks(index);

    while (so->scan_block < nblocks) {
        if (so->currbuf == kInvalidBuffer) {
            if (so->scan_block >= nblocks)
                break;

            so->currbuf = ReadBuffer(index->rd_smgr, ForkNumber::kMain, so->scan_block,
                                     ReadBufferMode::kNormal);
            Page page = BufferGetPage(so->currbuf);

            // Skip non-leaf pages.
            if (!(_bt_page_getopaque(page)->btpo_flags & kSpgistLeaf)) {
                ReleaseBuffer(so->currbuf);
                so->currbuf = kInvalidBuffer;
                so->scan_block++;
                continue;
            }
            so->curroff = 1;
        }

        Page page = BufferGetPage(so->currbuf);
        OffsetNumber max_off = PageGetMaxOffsetNumber(page);

        while (so->curroff <= max_off) {
            auto* item_id = PageGetItemId(page, so->curroff);
            so->curroff++;

            if (!ItemIdIsNormal(item_id))
                continue;

            auto* item = reinterpret_cast<BTItem>(PageGetItem(page, item_id));
            int32_t entry_key = spgist_leaf_get_key(item);

            if (spgist_entry_matches(entry_key, scan->scan_key)) {
                scan->curr_tid = item->tid;
                return true;
            }
        }

        ReleaseBuffer(so->currbuf);
        so->currbuf = kInvalidBuffer;
        so->scan_block++;
    }

    return false;
}

void spgistrescan(BTScanDesc scan, const BTScanKeyData* new_scan_key) {
    auto* so = static_cast<SpgistScanOpaque*>(scan->opaque);
    if (so != nullptr) {
        if (so->currbuf != kInvalidBuffer) {
            ReleaseBuffer(so->currbuf);
            so->currbuf = kInvalidBuffer;
        }
        so->inited = false;
        so->curroff = 0;
        so->scan_block = 0;
    }
    if (new_scan_key != nullptr) {
        scan->scan_key = *new_scan_key;
    }
}

void spgistendscan(BTScanDesc scan) {
    if (scan == nullptr)
        return;

    auto* so = static_cast<SpgistScanOpaque*>(scan->opaque);
    if (so != nullptr) {
        if (so->currbuf != kInvalidBuffer) {
            ReleaseBuffer(so->currbuf);
            so->currbuf = kInvalidBuffer;
        }
        destroyPallocNode(so);
    }
    destroyPallocNode(scan);
}

bool spgistcanreturn([[maybe_unused]] Relation index) {
    // SP-GiST can return heap tuples in PostgreSQL, but pgcpp simplifies to false.
    return false;
}

int64_t spgistgetbitmap(BTScanDesc scan, std::vector<ItemPointerData>* tids) {
    if (scan == nullptr || tids == nullptr)
        return 0;
    int64_t count = 0;
    while (spgistgettuple(scan)) {
        tids->push_back(scan->curr_tid);
        count++;
    }
    return count;
}

}  // namespace pgcpp::access

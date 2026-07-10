// gist.cpp — GiST (Generalized Search Tree) access method implementation.
//
// Converted from PostgreSQL 15's src/backend/access/gist/gist.c et al.
//
// GiST is a balanced search tree that supports extensible predicates.
// Each internal node stores bounding keys (min/max ranges) that describe
// the subtree's contents. Insertion descends to the best leaf and
// propagates splits upward.
//
// pgcpp simplifications:
//   - Supports int32 range keys (internal nodes store [min, max])
//   - Root starts as leaf; splits into two leaves + new internal root
//   - No VACUUM, no buffering build, no coverage splits
//   - Leaf pages chained via btpo_next for forward scanning
//
// Page layout:
//   Block 0: meta page (root block number)
//   Block 1: root page (leaf or internal)
//   Leaf entries: {ItemPointerData tid; int32_t key}
//   Internal entries: {BlockNumber child; int32_t min_key; int32_t max_key}

#include "access/gist.hpp"

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

// GiST page flags (stored in BTPageOpaqueData::btpo_flags).
constexpr uint32_t kGistMeta = 0x0100;      // meta page
constexpr uint32_t kGistLeaf = 0x0200;      // leaf page
constexpr uint32_t kGistRoot = 0x0400;      // root page
constexpr uint32_t kGistInternal = 0x0800;  // internal page

// GiST meta page magic.
constexpr uint32_t kGistMagic = 0x0683;

// GiST meta page data (stored in the content area of block 0).
struct GistMetaPageData {
    uint32_t magic = 0;
    BlockNumber root = 0;
};

// GiST leaf entry: {tid, key}. Reuses BTItemData layout.
// GiST internal entry: {child_block, min_key, max_key}.
struct GistInternalEntry {
    BlockNumber child = 0;
    int32_t min_key = 0;
    int32_t max_key = 0;
};

// Per-scan opaque state.
struct GistScanOpaque {
    Buffer currbuf = kInvalidBuffer;
    OffsetNumber curroff = 0;
    bool inited = false;
};

// --- Meta page helpers ---

void gist_init_meta_page(Page page, BlockNumber root) {
    PageInit(page, kBlckSz, kBTPageOpaqueSize);
    auto* opaque = _bt_page_getopaque(page);
    opaque->btpo_flags = kGistMeta;
    auto* meta = reinterpret_cast<GistMetaPageData*>(page + kPageHeaderSize);
    meta->magic = kGistMagic;
    meta->root = root;
}

GistMetaPageData gist_get_meta(Page page) {
    GistMetaPageData meta;
    std::memcpy(&meta, page + kPageHeaderSize, sizeof(meta));
    return meta;
}

void gist_update_meta(Page page, BlockNumber root) {
    auto* meta = reinterpret_cast<GistMetaPageData*>(page + kPageHeaderSize);
    meta->root = root;
}

// --- Page creation ---

Buffer gist_create_page(Relation index, BlockNumber block_num, uint32_t flags) {
    char pagebuf[kBlckSz];
    PageInit(pagebuf, kBlckSz, kBTPageOpaqueSize);
    auto* opaque = _bt_page_getopaque(pagebuf);
    opaque->btpo_flags = flags;
    opaque->btpo_next = 0;
    opaque->btpo_prev = 0;
    index->rd_smgr = RelationGetSmgr(index);
    pgcpp::storage::smgrextend(index->rd_smgr, ForkNumber::kMain, block_num, pagebuf, false);
    return ReadBuffer(index->rd_smgr, ForkNumber::kMain, block_num, ReadBufferMode::kNormal);
}

// Read a leaf entry's key.
int32_t gist_leaf_get_key(BTItem item) {
    int32_t key;
    std::memcpy(&key, _bt_item_get_key(item), sizeof(key));
    return key;
}

// Check if a leaf entry matches the scan key.
bool gist_entry_matches(int32_t entry_key, const BTScanKeyData& scan_key) {
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

}  // namespace

// --- Index creation ---

void gistbuild(Relation index, [[maybe_unused]] BTKeyKind key_kind) {
    index->rd_smgr = RelationGetSmgr(index);

    // Block 0: meta page (root = block 1).
    char metabuf[kBlckSz];
    gist_init_meta_page(metabuf, 1);
    pgcpp::storage::smgrextend(index->rd_smgr, ForkNumber::kMain, 0, metabuf, false);

    // Block 1: root leaf page.
    char rootbuf[kBlckSz];
    PageInit(rootbuf, kBlckSz, kBTPageOpaqueSize);
    auto* opaque = _bt_page_getopaque(rootbuf);
    opaque->btpo_flags = kGistLeaf | kGistRoot;
    opaque->btpo_next = 0;
    pgcpp::storage::smgrextend(index->rd_smgr, ForkNumber::kMain, 1, rootbuf, false);
}

// --- Insertion ---

bool gistinsert(Relation index, BTKeyKind kind, const void* key, [[maybe_unused]] uint16_t key_len,
                const ItemPointerData& tid) {
    if (kind != BTKeyKind::kInt32)
        return false;

    index->rd_smgr = RelationGetSmgr(index);
    int32_t key_val;
    std::memcpy(&key_val, key, sizeof(key_val));

    // Read meta page for root.
    Buffer meta_buf = ReadBuffer(index->rd_smgr, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
    Page meta_page = BufferGetPage(meta_buf);
    GistMetaPageData meta = gist_get_meta(meta_page);
    ReleaseBuffer(meta_buf);

    BlockNumber root_block = meta.root;
    Buffer root_buf =
        ReadBuffer(index->rd_smgr, ForkNumber::kMain, root_block, ReadBufferMode::kNormal);
    Page root_page = BufferGetPage(root_buf);

    // Build the leaf entry (same format as btree: tid + int32 key).
    uint16_t item_size = _bt_item_size(kind, key, key_len);
    char item_buf[256];
    _bt_build_item(reinterpret_cast<BTItem>(item_buf), kind, key, key_len, tid);

    if (_bt_page_getopaque(root_page)->btpo_flags & kGistLeaf) {
        // Root is a leaf — insert directly.
        OffsetNumber result = PageAddItem(root_page, reinterpret_cast<Item>(item_buf), item_size,
                                          kInvalidOffsetNumber, false);

        if (result != kInvalidOffsetNumber) {
            MarkBufferDirty(root_buf);
            ReleaseBuffer(root_buf);
            return true;
        }

        // Page is full — split into two leaves + new internal root.
        // Read all entries.
        std::vector<std::vector<char>> entries;
        OffsetNumber max_off = PageGetMaxOffsetNumber(root_page);
        for (OffsetNumber off = 1; off <= max_off; off++) {
            auto* item_id = PageGetItemId(root_page, off);
            if (!ItemIdIsNormal(item_id))
                continue;
            uint16_t len = ItemIdGetLength(item_id);
            const char* data = PageGetItem(root_page, item_id);
            entries.emplace_back(data, data + len);
        }

        // Add the new entry.
        entries.emplace_back(item_buf, item_buf + item_size);

        // Sort by key.
        std::sort(
            entries.begin(), entries.end(),
            [](const std::vector<char>& a, const std::vector<char>& b) {
                int32_t ka, kb;
                std::memcpy(&ka,
                            _bt_item_get_key(reinterpret_cast<BTItem>(const_cast<char*>(a.data()))),
                            sizeof(ka));
                std::memcpy(&kb,
                            _bt_item_get_key(reinterpret_cast<BTItem>(const_cast<char*>(b.data()))),
                            sizeof(kb));
                return ka < kb;
            });

        // Split at the midpoint.
        size_t mid = entries.size() / 2;

        BlockNumber nblocks = RelationGetNumberOfBlocks(index);
        BlockNumber new_leaf_block = nblocks;
        BlockNumber new_root_block = nblocks + 1;

        // Create the new leaf (right half).
        Buffer new_leaf_buf = gist_create_page(index, new_leaf_block, kGistLeaf);
        Page new_leaf_page = BufferGetPage(new_leaf_buf);
        for (size_t i = mid; i < entries.size(); i++) {
            PageAddItem(new_leaf_page, reinterpret_cast<Item>(const_cast<char*>(entries[i].data())),
                        entries[i].size(), kInvalidOffsetNumber, false);
        }

        // Reinitialize the old root leaf with the left half.
        PageInit(root_page, kBlckSz, kBTPageOpaqueSize);
        auto* old_opaque = _bt_page_getopaque(root_page);
        old_opaque->btpo_flags = kGistLeaf;
        for (size_t i = 0; i < mid; i++) {
            PageAddItem(root_page, reinterpret_cast<Item>(const_cast<char*>(entries[i].data())),
                        entries[i].size(), kInvalidOffsetNumber, false);
        }

        // Link the leaves.
        old_opaque->btpo_next = new_leaf_block;
        auto* new_opaque = _bt_page_getopaque(new_leaf_page);
        new_opaque->btpo_prev = root_block;

        MarkBufferDirty(root_buf);
        MarkBufferDirty(new_leaf_buf);
        ReleaseBuffer(new_leaf_buf);

        // Create the new internal root.
        Buffer new_root_buf = gist_create_page(index, new_root_block, kGistRoot | kGistInternal);
        Page new_root_page = BufferGetPage(new_root_buf);

        // Left child entry: {child=root_block, min, max} from left half.
        {
            GistInternalEntry entry;
            entry.child = root_block;
            std::memcpy(
                &entry.min_key,
                _bt_item_get_key(reinterpret_cast<BTItem>(const_cast<char*>(entries[0].data()))),
                sizeof(int32_t));
            std::memcpy(&entry.max_key,
                        _bt_item_get_key(
                            reinterpret_cast<BTItem>(const_cast<char*>(entries[mid - 1].data()))),
                        sizeof(int32_t));
            PageAddItem(new_root_page, reinterpret_cast<Item>(&entry), sizeof(entry),
                        kInvalidOffsetNumber, false);
        }
        // Right child entry: {child=new_leaf_block, min, max} from right half.
        {
            GistInternalEntry entry;
            entry.child = new_leaf_block;
            std::memcpy(
                &entry.min_key,
                _bt_item_get_key(reinterpret_cast<BTItem>(const_cast<char*>(entries[mid].data()))),
                sizeof(int32_t));
            std::memcpy(&entry.max_key,
                        _bt_item_get_key(reinterpret_cast<BTItem>(
                            const_cast<char*>(entries[entries.size() - 1].data()))),
                        sizeof(int32_t));
            PageAddItem(new_root_page, reinterpret_cast<Item>(&entry), sizeof(entry),
                        kInvalidOffsetNumber, false);
        }

        MarkBufferDirty(new_root_buf);
        ReleaseBuffer(new_root_buf);
        ReleaseBuffer(root_buf);

        // Update meta page with new root.
        Buffer meta_buf2 =
            ReadBuffer(index->rd_smgr, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
        Page meta_page2 = BufferGetPage(meta_buf2);
        gist_update_meta(meta_page2, new_root_block);
        MarkBufferDirty(meta_buf2);
        ReleaseBuffer(meta_buf2);

        return true;
    }

    // Root is internal — descend to the right leaf.
    OffsetNumber max_off = PageGetMaxOffsetNumber(root_page);
    BlockNumber child_block = root_block;

    for (OffsetNumber off = 1; off <= max_off; off++) {
        auto* item_id = PageGetItemId(root_page, off);
        if (!ItemIdIsNormal(item_id))
            continue;
        auto* entry = reinterpret_cast<GistInternalEntry*>(PageGetItem(root_page, item_id));
        if (key_val >= entry->min_key && key_val <= entry->max_key) {
            child_block = entry->child;
            break;
        }
        child_block = entry->child;  // fallback: last entry
    }

    ReleaseBuffer(root_buf);

    // Insert into the leaf.
    Buffer leaf_buf =
        ReadBuffer(index->rd_smgr, ForkNumber::kMain, child_block, ReadBufferMode::kNormal);
    Page leaf_page = BufferGetPage(leaf_buf);
    OffsetNumber result = PageAddItem(leaf_page, reinterpret_cast<Item>(item_buf), item_size,
                                      kInvalidOffsetNumber, false);

    if (result != kInvalidOffsetNumber) {
        MarkBufferDirty(leaf_buf);
        ReleaseBuffer(leaf_buf);
        return true;
    }

    // Leaf is full — simplified: just warn (no leaf split for internal root).
    ReleaseBuffer(leaf_buf);
    ereport(pgcpp::error::LogLevel::kWarning,
            "gistinsert: leaf page is full, split not implemented for internal root");
    return false;
}

// --- Scan ---

BTScanDesc gistbeginscan(Relation index, BTKeyKind kind, const BTScanKeyData* scan_key) {
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

    auto* opaque = makePallocNode<GistScanOpaque>();
    scan->opaque = opaque;
    return scan;
}

bool gistgettuple(BTScanDesc scan) {
    Relation index = scan->index;
    index->rd_smgr = RelationGetSmgr(index);
    auto* so = static_cast<GistScanOpaque*>(scan->opaque);
    if (so == nullptr)
        return false;

    if (!so->inited) {
        so->inited = true;

        // Start at the root.
        Buffer meta_buf = ReadBuffer(index->rd_smgr, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
        Page meta_page = BufferGetPage(meta_buf);
        GistMetaPageData meta = gist_get_meta(meta_page);
        ReleaseBuffer(meta_buf);

        Buffer buf =
            ReadBuffer(index->rd_smgr, ForkNumber::kMain, meta.root, ReadBufferMode::kNormal);
        Page page = BufferGetPage(buf);

        // If root is internal, descend to the leftmost leaf for full scans,
        // or to the best matching leaf for key-based scans.
        if (!(_bt_page_getopaque(page)->btpo_flags & kGistLeaf)) {
            OffsetNumber max_off = PageGetMaxOffsetNumber(page);
            BlockNumber child = meta.root;

            if (scan->scan_key.strategy != BTStrategy::kAll && scan->scan_key.key != nullptr) {
                int32_t key_val;
                std::memcpy(&key_val, scan->scan_key.key, sizeof(key_val));
                for (OffsetNumber off = 1; off <= max_off; off++) {
                    auto* item_id = PageGetItemId(page, off);
                    if (!ItemIdIsNormal(item_id))
                        continue;
                    auto* entry = reinterpret_cast<GistInternalEntry*>(PageGetItem(page, item_id));
                    if (key_val >= entry->min_key && key_val <= entry->max_key) {
                        child = entry->child;
                        break;
                    }
                    child = entry->child;
                }
            } else {
                // Full scan: start at the leftmost child.
                auto* item_id = PageGetItemId(page, 1);
                if (ItemIdIsNormal(item_id)) {
                    auto* entry = reinterpret_cast<GistInternalEntry*>(PageGetItem(page, item_id));
                    child = entry->child;
                }
            }

            ReleaseBuffer(buf);
            buf = ReadBuffer(index->rd_smgr, ForkNumber::kMain, child, ReadBufferMode::kNormal);
        }

        so->currbuf = buf;
        so->curroff = 1;
    }

    // Scan forward through leaf entries.
    while (so->currbuf != kInvalidBuffer) {
        Page page = BufferGetPage(so->currbuf);
        OffsetNumber max_off = PageGetMaxOffsetNumber(page);

        while (so->curroff <= max_off) {
            auto* item_id = PageGetItemId(page, so->curroff);
            so->curroff++;

            if (!ItemIdIsNormal(item_id))
                continue;

            auto* item = reinterpret_cast<BTItem>(PageGetItem(page, item_id));
            int32_t entry_key = gist_leaf_get_key(item);

            if (gist_entry_matches(entry_key, scan->scan_key)) {
                scan->curr_tid = item->tid;
                return true;
            }
        }

        // Move to the next leaf page.
        BTPageOpaque opaque = _bt_page_getopaque(page);
        BlockNumber next = opaque->btpo_next;

        ReleaseBuffer(so->currbuf);
        so->currbuf = kInvalidBuffer;

        if (next != 0) {
            so->currbuf =
                ReadBuffer(index->rd_smgr, ForkNumber::kMain, next, ReadBufferMode::kNormal);
            so->curroff = 1;
        }
    }

    return false;
}

void gistrescan(BTScanDesc scan, const BTScanKeyData* new_scan_key) {
    auto* so = static_cast<GistScanOpaque*>(scan->opaque);
    if (so != nullptr) {
        if (so->currbuf != kInvalidBuffer) {
            ReleaseBuffer(so->currbuf);
            so->currbuf = kInvalidBuffer;
        }
        so->inited = false;
        so->curroff = 0;
    }
    if (new_scan_key != nullptr) {
        scan->scan_key = *new_scan_key;
    }
}

void gistendscan(BTScanDesc scan) {
    if (scan == nullptr)
        return;

    auto* so = static_cast<GistScanOpaque*>(scan->opaque);
    if (so != nullptr) {
        if (so->currbuf != kInvalidBuffer) {
            ReleaseBuffer(so->currbuf);
            so->currbuf = kInvalidBuffer;
        }
        destroyPallocNode(so);
    }
    destroyPallocNode(scan);
}

bool gistcanreturn([[maybe_unused]] Relation index) {
    // GiST can return heap tuples in PostgreSQL, but pgcpp simplifies to false.
    return false;
}

int64_t gistgetbitmap(BTScanDesc scan, std::vector<ItemPointerData>* tids) {
    if (scan == nullptr || tids == nullptr)
        return 0;
    int64_t count = 0;
    while (gistgettuple(scan)) {
        tids->push_back(scan->curr_tid);
        count++;
    }
    return count;
}

}  // namespace pgcpp::access

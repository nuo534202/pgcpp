// nbtree.cpp — B-tree index access method implementation.
//
// Converted from PostgreSQL 15's src/backend/access/nbtree/nbtree.c.
//
// Implements btinsert, btbeginscan, btgettuple, btrescan, and btbuild.
// The B-tree uses pages for storage with sorted entries on leaf pages.
//
// MyToyDB simplifications:
//   - Root starts as a leaf; splits into two leaves + new internal root
//   - No further splitting of internal pages (limited index size)
//   - No VACUUM, no deduplication, no page deletion
//   - Supports int32, int64, and text keys

#include "mytoydb/access/nbtree.hpp"

#include <cstring>
#include <string>
#include <vector>

#include "mytoydb/access/nbtpage.hpp"
#include "mytoydb/access/rel.hpp"
#include "mytoydb/common/containers/node.hpp"
#include "mytoydb/common/error/elog.hpp"
#include "mytoydb/common/memory/memory_context.hpp"
#include "mytoydb/storage/bufmgr.hpp"
#include "mytoydb/storage/bufpage.hpp"
#include "mytoydb/storage/smgr.hpp"
#include "mytoydb/transaction/heap_tuple.hpp"

namespace mytoydb::access {
using mytoydb::nodes::destroyPallocNode;
using mytoydb::nodes::makePallocNode;

namespace {

using mytoydb::memory::palloc;
using mytoydb::memory::pfree;
using mytoydb::storage::BlockNumber;
using mytoydb::storage::Buffer;
using mytoydb::storage::BufferGetPage;
using mytoydb::storage::ForkNumber;
using mytoydb::storage::Item;
using mytoydb::storage::ItemIdData;
using mytoydb::storage::ItemIdGetLength;
using mytoydb::storage::ItemIdIsNormal;
using mytoydb::storage::kBlckSz;
using mytoydb::storage::kInvalidBuffer;
using mytoydb::storage::kInvalidOffsetNumber;
using mytoydb::storage::kPageHeaderSize;
using mytoydb::storage::MarkBufferDirty;
using mytoydb::storage::OffsetNumber;
using mytoydb::storage::Page;
using mytoydb::storage::PageAddItem;
using mytoydb::storage::PageGetHeapFreeSpace;
using mytoydb::storage::PageGetItem;
using mytoydb::storage::PageGetItemId;
using mytoydb::storage::PageGetMaxOffsetNumber;
using mytoydb::storage::PageInit;
using mytoydb::storage::ReadBuffer;
using mytoydb::storage::ReadBufferMode;
using mytoydb::storage::ReleaseBuffer;
using mytoydb::transaction::ItemPointerData;

// Initialize a new B-tree page and extend the relation.
Buffer _bt_create_page(Relation index, BlockNumber block_num, uint32_t flags, uint32_t level) {
    char pagebuf[kBlckSz];
    _bt_init_page(pagebuf, flags, level);
    index->rd_smgr = RelationGetSmgr(index);
    mytoydb::storage::smgrextend(index->rd_smgr, ForkNumber::kMain, block_num, pagebuf, false);
    return ReadBuffer(index->rd_smgr, ForkNumber::kMain, block_num, ReadBufferMode::kNormal);
}

// Insert an item into a B-tree page at the specified offset, shifting
// existing line pointers. Returns the offset, or kInvalidOffsetNumber
// if the page is full.
OffsetNumber _bt_page_insert_item(Page page, const void* item_data, uint16_t item_size,
                                  OffsetNumber insert_offset) {
    auto* phdr = reinterpret_cast<mytoydb::storage::PageHeader>(page);
    OffsetNumber max_offset = PageGetMaxOffsetNumber(page);

    // Check if there's enough free space.
    int aligned_size = (item_size + 3) & ~3;
    int space_needed = aligned_size + static_cast<int>(sizeof(ItemIdData));
    if (PageGetHeapFreeSpace(page) < space_needed) {
        return kInvalidOffsetNumber;
    }

    // Shift line pointers after insert_offset down by one slot.
    // Line pointer array: lp[0] = offset 1, lp[1] = offset 2, etc.
    if (insert_offset <= max_offset) {
        auto* lp = reinterpret_cast<ItemIdData*>(page + kPageHeaderSize);
        std::memmove(&lp[insert_offset], &lp[insert_offset - 1],
                     (max_offset - insert_offset + 1) * sizeof(ItemIdData));
    }

    // Place the new item data at pd_upper - aligned_size.
    int item_offset = phdr->pd_upper - aligned_size;
    std::memcpy(page + item_offset, item_data, item_size);

    // Set the line pointer for the new item.
    auto* new_lp = reinterpret_cast<ItemIdData*>(page + kPageHeaderSize) + (insert_offset - 1);
    new_lp->li_off = static_cast<uint32_t>(item_offset);
    new_lp->li_flags = mytoydb::storage::kLPNormal;
    new_lp->li_len = item_size;

    // Update header.
    phdr->pd_lower += sizeof(ItemIdData);
    phdr->pd_upper = static_cast<mytoydb::storage::LocationIndex>(item_offset);

    return insert_offset;
}

// Read all items from a leaf page into a vector (for splitting).
struct LeafEntry {
    std::vector<char> data;
    uint16_t size = 0;
};

std::vector<LeafEntry> _bt_read_all_items(Page page) {
    std::vector<LeafEntry> entries;
    OffsetNumber max_offset = PageGetMaxOffsetNumber(page);
    for (OffsetNumber off = 1; off <= max_offset; off++) {
        auto* item_id = PageGetItemId(page, off);
        if (!ItemIdIsNormal(item_id))
            continue;
        uint16_t len = ItemIdGetLength(item_id);
        const char* data = PageGetItem(page, item_id);
        LeafEntry entry;
        entry.size = len;
        entry.data.assign(data, data + len);
        entries.push_back(std::move(entry));
    }
    return entries;
}

}  // namespace

// --- Key comparison helpers ---

OffsetNumber _bt_find_insert_pos(Page page, BTKeyKind kind, const void* key, uint16_t key_len) {
    OffsetNumber max_offset = PageGetMaxOffsetNumber(page);
    for (OffsetNumber off = 1; off <= max_offset; off++) {
        auto* item_id = PageGetItemId(page, off);
        if (!ItemIdIsNormal(item_id))
            continue;

        auto* bt_item = reinterpret_cast<BTItem>(PageGetItem(page, item_id));
        const void* item_key = _bt_item_get_key(bt_item);
        uint16_t item_key_len = _bt_item_get_key_len(ItemIdGetLength(item_id));

        int cmp = _bt_compare_keys(kind, key, key_len, item_key, item_key_len);
        if (cmp <= 0) {
            return off;  // Insert before this item.
        }
    }
    return max_offset + 1;  // Insert at the end.
}

OffsetNumber _bt_find_scan_pos(Page page, BTKeyKind kind, const void* key, uint16_t key_len) {
    OffsetNumber max_offset = PageGetMaxOffsetNumber(page);
    for (OffsetNumber off = 1; off <= max_offset; off++) {
        auto* item_id = PageGetItemId(page, off);
        if (!ItemIdIsNormal(item_id))
            continue;

        auto* bt_item = reinterpret_cast<BTItem>(PageGetItem(page, item_id));
        const void* item_key = _bt_item_get_key(bt_item);
        uint16_t item_key_len = _bt_item_get_key_len(ItemIdGetLength(item_id));

        int cmp = _bt_compare_keys(kind, key, key_len, item_key, item_key_len);
        if (cmp <= 0) {
            return off;  // Found first entry >= key.
        }
    }
    return max_offset + 1;  // Past the end.
}

// --- Index creation ---

void btbuild(Relation index, BTKeyKind key_kind) {
    index->rd_smgr = RelationGetSmgr(index);

    // Block 0: meta page.
    char metabuf[kBlckSz];
    _bt_init_meta_page(metabuf, 1);  // root = block 1
    mytoydb::storage::smgrextend(index->rd_smgr, ForkNumber::kMain, 0, metabuf, false);

    // Block 1: root leaf page.
    char rootbuf[kBlckSz];
    _bt_init_page(rootbuf, kBtpLeaf | kBtpRoot, 0);
    mytoydb::storage::smgrextend(index->rd_smgr, ForkNumber::kMain, 1, rootbuf, false);
}

// --- Insertion ---

bool btinsert(Relation index, BTKeyKind kind, const void* key, uint16_t key_len,
              const ItemPointerData& tid) {
    index->rd_smgr = RelationGetSmgr(index);

    // Read meta page to find root.
    Buffer meta_buf = ReadBuffer(index->rd_smgr, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
    Page meta_page = BufferGetPage(meta_buf);
    BTMetaPageData meta = _bt_get_meta(meta_page);
    ReleaseBuffer(meta_buf);

    BlockNumber root_block = meta.root;
    Buffer root_buf =
        ReadBuffer(index->rd_smgr, ForkNumber::kMain, root_block, ReadBufferMode::kNormal);
    Page root_page = BufferGetPage(root_buf);

    // Build the index entry.
    uint16_t item_size = _bt_item_size(kind, key, key_len);
    char item_buf[256];  // Enough for tid + any key type.
    _bt_build_item(reinterpret_cast<BTItem>(item_buf), kind, key, key_len, tid);

    if (_bt_is_leaf_page(root_page)) {
        // Root is a leaf — insert directly.
        OffsetNumber insert_pos = _bt_find_insert_pos(root_page, kind, key, key_len);
        OffsetNumber result = _bt_page_insert_item(root_page, item_buf, item_size, insert_pos);

        if (result != kInvalidOffsetNumber) {
            MarkBufferDirty(root_buf);
            ReleaseBuffer(root_buf);
            return true;
        }

        // Page is full — need to split.
        // Read all items from the root.
        std::vector<LeafEntry> entries = _bt_read_all_items(root_page);

        // Add the new item in sorted position.
        LeafEntry new_entry;
        new_entry.size = item_size;
        new_entry.data.assign(item_buf, item_buf + item_size);

        // Find insertion position in the entries vector.
        size_t insert_idx = entries.size();
        for (size_t i = 0; i < entries.size(); i++) {
            auto* bt_item = reinterpret_cast<BTItem>(entries[i].data.data());
            const void* item_key = _bt_item_get_key(bt_item);
            uint16_t item_key_len = _bt_item_get_key_len(entries[i].size);
            int cmp = _bt_compare_keys(kind, key, key_len, item_key, item_key_len);
            if (cmp <= 0) {
                insert_idx = i;
                break;
            }
        }
        entries.insert(entries.begin() + insert_idx, std::move(new_entry));

        // Split at the midpoint.
        size_t mid = entries.size() / 2;

        // Get the current number of blocks (for new page allocation).
        BlockNumber nblocks = RelationGetNumberOfBlocks(index);
        BlockNumber new_leaf_block = nblocks;
        BlockNumber new_root_block = nblocks + 1;

        // Create the new leaf page (right half).
        Buffer new_leaf_buf = _bt_create_page(index, new_leaf_block, kBtpLeaf, 0);
        Page new_leaf_page = BufferGetPage(new_leaf_buf);

        // Reinitialize the old root leaf with the left half.
        _bt_init_page(root_page, kBtpLeaf, 0);

        // Write left half to old root.
        for (size_t i = 0; i < mid; i++) {
            PageAddItem(root_page, reinterpret_cast<Item>(entries[i].data.data()), entries[i].size,
                        kInvalidOffsetNumber, false);
        }

        // Write right half to new leaf.
        for (size_t i = mid; i < entries.size(); i++) {
            PageAddItem(new_leaf_page, reinterpret_cast<Item>(entries[i].data.data()),
                        entries[i].size, kInvalidOffsetNumber, false);
        }

        // Link the leaves.
        BTPageOpaque old_opaque = _bt_page_getopaque(root_page);
        BTPageOpaque new_opaque = _bt_page_getopaque(new_leaf_page);
        old_opaque->btpo_next = new_leaf_block;
        new_opaque->btpo_prev = root_block;

        MarkBufferDirty(root_buf);
        MarkBufferDirty(new_leaf_buf);
        ReleaseBuffer(new_leaf_buf);

        // Create the new internal root.
        Buffer new_root_buf = _bt_create_page(index, new_root_block, kBtpRoot, 1);
        Page new_root_page = BufferGetPage(new_root_buf);

        // Internal entry format: {BlockNumber child; key_data}
        // First entry: child = old root (leftmost), no key (key_len = 0).
        {
            struct {
                BlockNumber child;
            } entry;
            entry.child = root_block;
            PageAddItem(new_root_page, reinterpret_cast<Item>(&entry), sizeof(entry),
                        kInvalidOffsetNumber, false);
        }

        // Second entry: child = new leaf, key = first key in new leaf.
        {
            auto* first_item = reinterpret_cast<BTItem>(entries[mid].data.data());
            const void* first_key = _bt_item_get_key(first_item);
            uint16_t first_key_len = _bt_item_get_key_len(entries[mid].size);

            uint16_t int_entry_size = sizeof(BlockNumber) + first_key_len;
            std::vector<char> int_entry(int_entry_size);
            BlockNumber child = new_leaf_block;
            std::memcpy(int_entry.data(), &child, sizeof(BlockNumber));
            std::memcpy(int_entry.data() + sizeof(BlockNumber), first_key, first_key_len);
            PageAddItem(new_root_page, reinterpret_cast<Item>(int_entry.data()), int_entry_size,
                        kInvalidOffsetNumber, false);
        }

        MarkBufferDirty(new_root_buf);
        ReleaseBuffer(new_root_buf);
        ReleaseBuffer(root_buf);

        // Update meta page with new root.
        Buffer meta_buf2 =
            ReadBuffer(index->rd_smgr, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
        Page meta_page2 = BufferGetPage(meta_buf2);
        _bt_update_meta(meta_page2, new_root_block);
        MarkBufferDirty(meta_buf2);
        ReleaseBuffer(meta_buf2);

        return true;
    }

    // Root is internal — descend to the right leaf.
    OffsetNumber max_offset = PageGetMaxOffsetNumber(root_page);
    BlockNumber child_block = root_block;  // Default: leftmost child.

    for (OffsetNumber off = 1; off <= max_offset; off++) {
        auto* item_id = PageGetItemId(root_page, off);
        if (!ItemIdIsNormal(item_id))
            continue;

        const char* data = PageGetItem(root_page, item_id);
        uint16_t data_len = ItemIdGetLength(item_id);

        if (data_len < sizeof(BlockNumber))
            continue;

        BlockNumber entry_child;
        std::memcpy(&entry_child, data, sizeof(BlockNumber));

        // First entry (offset 1) has no key — always the leftmost child.
        if (off == 1) {
            child_block = entry_child;
            continue;
        }

        // Compare search key with this entry's key.
        const void* entry_key = data + sizeof(BlockNumber);
        uint16_t entry_key_len = data_len - sizeof(BlockNumber);
        int cmp = _bt_compare_keys(kind, key, key_len, entry_key, entry_key_len);
        if (cmp < 0) {
            // Search key < this entry's key → go to previous child.
            break;
        }
        // Search key >= this entry's key → this is the right child so far.
        child_block = entry_child;
    }

    // Note: root_buf is kept pinned — needed if the leaf must split.

    // Read the leaf page and insert.
    Buffer leaf_buf =
        ReadBuffer(index->rd_smgr, ForkNumber::kMain, child_block, ReadBufferMode::kNormal);
    Page leaf_page = BufferGetPage(leaf_buf);

    OffsetNumber insert_pos = _bt_find_insert_pos(leaf_page, kind, key, key_len);
    OffsetNumber result = _bt_page_insert_item(leaf_page, item_buf, item_size, insert_pos);

    if (result != kInvalidOffsetNumber) {
        MarkBufferDirty(leaf_buf);
        ReleaseBuffer(leaf_buf);
        ReleaseBuffer(root_buf);
        return true;
    }

    // Leaf is full — split it into two leaves and add a separator entry
    // to the internal root.
    std::vector<LeafEntry> entries = _bt_read_all_items(leaf_page);

    // Add the new item in sorted position.
    LeafEntry new_entry;
    new_entry.size = item_size;
    new_entry.data.assign(item_buf, item_buf + item_size);

    size_t insert_idx = entries.size();
    for (size_t i = 0; i < entries.size(); i++) {
        auto* bt_item = reinterpret_cast<BTItem>(entries[i].data.data());
        const void* item_key = _bt_item_get_key(bt_item);
        uint16_t item_key_len = _bt_item_get_key_len(entries[i].size);
        int cmp = _bt_compare_keys(kind, key, key_len, item_key, item_key_len);
        if (cmp <= 0) {
            insert_idx = i;
            break;
        }
    }
    entries.insert(entries.begin() + insert_idx, std::move(new_entry));

    // Split at the midpoint.
    size_t mid = entries.size() / 2;

    // Save the old leaf's forward link before reinitializing.
    BTPageOpaque old_opaque = _bt_page_getopaque(leaf_page);
    BlockNumber old_next = old_opaque->btpo_next;

    // Allocate a new leaf block at the end of the relation.
    BlockNumber nblocks = RelationGetNumberOfBlocks(index);
    BlockNumber new_leaf_block = nblocks;

    // Reinitialize the old leaf with the left half.
    _bt_init_page(leaf_page, kBtpLeaf, 0);
    for (size_t i = 0; i < mid; i++) {
        PageAddItem(leaf_page, reinterpret_cast<Item>(entries[i].data.data()), entries[i].size,
                    kInvalidOffsetNumber, false);
    }

    // Create the new leaf page with the right half.
    Buffer new_leaf_buf = _bt_create_page(index, new_leaf_block, kBtpLeaf, 0);
    Page new_leaf_page = BufferGetPage(new_leaf_buf);
    for (size_t i = mid; i < entries.size(); i++) {
        PageAddItem(new_leaf_page, reinterpret_cast<Item>(entries[i].data.data()), entries[i].size,
                    kInvalidOffsetNumber, false);
    }

    // Link the leaves into the forward chain.
    old_opaque = _bt_page_getopaque(leaf_page);
    BTPageOpaque new_opaque = _bt_page_getopaque(new_leaf_page);
    old_opaque->btpo_next = new_leaf_block;
    new_opaque->btpo_prev = child_block;
    new_opaque->btpo_next = old_next;

    MarkBufferDirty(leaf_buf);
    MarkBufferDirty(new_leaf_buf);
    ReleaseBuffer(new_leaf_buf);
    ReleaseBuffer(leaf_buf);

    // Add a separator entry to the internal root pointing to the new leaf.
    // The separator key is the first key in the right half.
    auto* first_item = reinterpret_cast<BTItem>(entries[mid].data.data());
    const void* first_key = _bt_item_get_key(first_item);
    uint16_t first_key_len = _bt_item_get_key_len(entries[mid].size);

    uint16_t int_entry_size = sizeof(BlockNumber) + first_key_len;
    std::vector<char> int_entry(int_entry_size);
    BlockNumber new_child = new_leaf_block;
    std::memcpy(int_entry.data(), &new_child, sizeof(BlockNumber));
    std::memcpy(int_entry.data() + sizeof(BlockNumber), first_key, first_key_len);

    // Find the insertion position in the internal root by comparing
    // separator keys. Offset 1 is the leftmost child (no key), so we
    // start at offset 2.
    OffsetNumber root_insert_pos = kInvalidOffsetNumber;
    OffsetNumber root_max = PageGetMaxOffsetNumber(root_page);
    for (OffsetNumber off = 2; off <= root_max; off++) {
        auto* item_id = PageGetItemId(root_page, off);
        if (!ItemIdIsNormal(item_id))
            continue;
        const char* data = PageGetItem(root_page, item_id);
        uint16_t data_len = ItemIdGetLength(item_id);
        if (data_len < sizeof(BlockNumber))
            continue;
        const void* entry_key = data + sizeof(BlockNumber);
        uint16_t entry_key_len = data_len - sizeof(BlockNumber);
        int cmp = _bt_compare_keys(kind, first_key, first_key_len, entry_key, entry_key_len);
        if (cmp <= 0) {
            root_insert_pos = off;
            break;
        }
    }
    if (root_insert_pos == kInvalidOffsetNumber) {
        root_insert_pos = root_max + 1;
    }

    OffsetNumber root_result =
        _bt_page_insert_item(root_page, int_entry.data(), int_entry_size, root_insert_pos);
    if (root_result == kInvalidOffsetNumber) {
        // Internal root is full — simplification: no internal page splitting.
        ereport(mytoydb::error::LogLevel::kWarning,
                "btinsert: internal root is full, splitting not implemented");
    }
    MarkBufferDirty(root_buf);
    ReleaseBuffer(root_buf);

    return true;
}

// --- Scan ---

Buffer _bt_search_leaf(Relation index, BTKeyKind kind, const void* key, uint16_t key_len,
                       OffsetNumber* offset) {
    index->rd_smgr = RelationGetSmgr(index);

    // Read meta page to find root.
    Buffer meta_buf = ReadBuffer(index->rd_smgr, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
    Page meta_page = BufferGetPage(meta_buf);
    BTMetaPageData meta = _bt_get_meta(meta_page);
    ReleaseBuffer(meta_buf);

    BlockNumber root_block = meta.root;
    Buffer root_buf =
        ReadBuffer(index->rd_smgr, ForkNumber::kMain, root_block, ReadBufferMode::kNormal);
    Page root_page = BufferGetPage(root_buf);

    BlockNumber leaf_block;

    if (_bt_is_leaf_page(root_page)) {
        // Root is a leaf.
        leaf_block = root_block;
        *offset = _bt_find_scan_pos(root_page, kind, key, key_len);
        return root_buf;
    }

    // Root is internal — descend to the right leaf.
    OffsetNumber max_offset = PageGetMaxOffsetNumber(root_page);
    leaf_block = root_block;  // Default.

    for (OffsetNumber off = 1; off <= max_offset; off++) {
        auto* item_id = PageGetItemId(root_page, off);
        if (!ItemIdIsNormal(item_id))
            continue;

        const char* data = PageGetItem(root_page, item_id);
        uint16_t data_len = ItemIdGetLength(item_id);

        if (data_len < sizeof(BlockNumber))
            continue;

        BlockNumber entry_child;
        std::memcpy(&entry_child, data, sizeof(BlockNumber));

        if (off == 1) {
            leaf_block = entry_child;
            continue;
        }

        const void* entry_key = data + sizeof(BlockNumber);
        uint16_t entry_key_len = data_len - sizeof(BlockNumber);
        int cmp = _bt_compare_keys(kind, key, key_len, entry_key, entry_key_len);
        if (cmp < 0) {
            break;
        }
        leaf_block = entry_child;
    }

    ReleaseBuffer(root_buf);

    // Read the leaf page.
    Buffer leaf_buf =
        ReadBuffer(index->rd_smgr, ForkNumber::kMain, leaf_block, ReadBufferMode::kNormal);
    Page leaf_page = BufferGetPage(leaf_buf);
    *offset = _bt_find_scan_pos(leaf_page, kind, key, key_len);
    return leaf_buf;
}

BTScanDesc btbeginscan(Relation index, BTKeyKind kind, const BTScanKeyData* scan_key) {
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

    return scan;
}

// Check if an entry matches the scan strategy.
static bool _bt_entry_matches(BTKeyKind kind, const void* entry_key, uint16_t entry_key_len,
                              const BTScanKeyData& scan_key) {
    if (scan_key.strategy == BTStrategy::kAll || scan_key.key == nullptr) {
        return true;
    }

    int cmp = _bt_compare_keys(kind, entry_key, entry_key_len, scan_key.key, scan_key.key_len);

    switch (scan_key.strategy) {
        case BTStrategy::kEqual:
            return cmp == 0;
        case BTStrategy::kLess:
            return cmp < 0;
        case BTStrategy::kLessEqual:
            return cmp <= 0;
        case BTStrategy::kGreater:
            return cmp > 0;
        case BTStrategy::kGreaterEqual:
            return cmp >= 0;
        case BTStrategy::kAll:
            return true;
    }
    return false;
}

bool btgettuple(BTScanDesc scan) {
    Relation index = scan->index;
    index->rd_smgr = RelationGetSmgr(index);

    // If not initialized, position the scan at the starting leaf.
    if (!scan->inited) {
        scan->inited = true;

        bool start_from_beginning = scan->scan_key.strategy == BTStrategy::kAll ||
                                    scan->scan_key.strategy == BTStrategy::kLess ||
                                    scan->scan_key.strategy == BTStrategy::kLessEqual ||
                                    scan->scan_key.key == nullptr;

        if (start_from_beginning) {
            // Full scan or less-than scan: start at the leftmost leaf.
            Buffer meta_buf =
                ReadBuffer(index->rd_smgr, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
            Page meta_page = BufferGetPage(meta_buf);
            BTMetaPageData meta = _bt_get_meta(meta_page);
            ReleaseBuffer(meta_buf);

            scan->curr_block = meta.root;
            scan->curr_buf = ReadBuffer(index->rd_smgr, ForkNumber::kMain, scan->curr_block,
                                        ReadBufferMode::kNormal);

            // If root is internal, descend to the leftmost leaf.
            Page page = BufferGetPage(scan->curr_buf);
            if (!_bt_is_leaf_page(page)) {
                OffsetNumber max_off = PageGetMaxOffsetNumber(page);
                if (max_off >= 1) {
                    auto* item_id = PageGetItemId(page, 1);
                    const char* data = PageGetItem(page, item_id);
                    BlockNumber child;
                    std::memcpy(&child, data, sizeof(BlockNumber));
                    ReleaseBuffer(scan->curr_buf);
                    scan->curr_block = child;
                    scan->curr_buf = ReadBuffer(index->rd_smgr, ForkNumber::kMain, scan->curr_block,
                                                ReadBufferMode::kNormal);
                }
            }
            scan->curr_offset = 1;
        } else {
            // Key-based scan (equal/greater): find the starting position.
            OffsetNumber start_offset;
            scan->curr_buf = _bt_search_leaf(index, scan->key_kind, scan->scan_key.key,
                                             scan->scan_key.key_len, &start_offset);
            Page page = BufferGetPage(scan->curr_buf);
            scan->curr_block = 0;  // Will be set from the buffer's tag
            // Get the block number from the buffer descriptor.
            auto* pool = mytoydb::storage::GetBufferPool();
            auto* desc = pool->GetBufferDesc(scan->curr_buf);
            scan->curr_block = desc->tag.block_num;
            scan->curr_offset = start_offset;
        }
    }

    // Scan forward through entries.
    while (true) {
        if (scan->curr_buf == kInvalidBuffer) {
            return false;
        }

        Page page = BufferGetPage(scan->curr_buf);
        OffsetNumber max_offset = PageGetMaxOffsetNumber(page);

        while (scan->curr_offset <= max_offset) {
            auto* item_id = PageGetItemId(page, scan->curr_offset);
            scan->curr_offset++;

            if (!ItemIdIsNormal(item_id))
                continue;

            auto* bt_item = reinterpret_cast<BTItem>(PageGetItem(page, item_id));
            const void* entry_key = _bt_item_get_key(bt_item);
            uint16_t entry_key_len = _bt_item_get_key_len(ItemIdGetLength(item_id));

            if (_bt_entry_matches(scan->key_kind, entry_key, entry_key_len, scan->scan_key)) {
                scan->curr_tid = bt_item->tid;
                return true;
            }

            // Early termination for strategies with an upper bound.
            // B-tree entries are sorted, so once we pass the key we can stop.
            if (scan->scan_key.key != nullptr) {
                BTStrategy strat = scan->scan_key.strategy;
                bool has_upper_bound = strat == BTStrategy::kEqual || strat == BTStrategy::kLess ||
                                       strat == BTStrategy::kLessEqual;
                if (has_upper_bound) {
                    int cmp = _bt_compare_keys(scan->key_kind, entry_key, entry_key_len,
                                               scan->scan_key.key, scan->scan_key.key_len);
                    // kEqual/kLessEqual: stop past the key (cmp > 0).
                    // kLess: stop at or past the key (cmp >= 0).
                    bool stop = (strat == BTStrategy::kLess) ? (cmp >= 0) : (cmp > 0);
                    if (stop) {
                        ReleaseBuffer(scan->curr_buf);
                        scan->curr_buf = kInvalidBuffer;
                        return false;
                    }
                }
            }
        }

        // Move to the next leaf page.
        BTPageOpaque opaque = _bt_page_getopaque(page);
        BlockNumber next_block = opaque->btpo_next;

        ReleaseBuffer(scan->curr_buf);
        scan->curr_buf = kInvalidBuffer;

        if (next_block == 0) {
            // No more pages.
            return false;
        }

        scan->curr_block = next_block;
        scan->curr_buf = ReadBuffer(index->rd_smgr, ForkNumber::kMain, scan->curr_block,
                                    ReadBufferMode::kNormal);
        scan->curr_offset = 1;
    }
}

void btrescan(BTScanDesc scan, const BTScanKeyData* new_scan_key) {
    if (scan->curr_buf != kInvalidBuffer) {
        ReleaseBuffer(scan->curr_buf);
        scan->curr_buf = kInvalidBuffer;
    }
    scan->inited = false;
    scan->curr_offset = 0;

    if (new_scan_key != nullptr) {
        scan->scan_key = *new_scan_key;
    }
}

void btendscan(BTScanDesc scan) {
    if (scan == nullptr)
        return;

    if (scan->curr_buf != kInvalidBuffer) {
        ReleaseBuffer(scan->curr_buf);
        scan->curr_buf = kInvalidBuffer;
    }

    destroyPallocNode(scan);
}

}  // namespace mytoydb::access

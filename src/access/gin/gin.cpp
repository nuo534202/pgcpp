// gin.cpp — GIN (Generalized Inverted Index) access method implementation.
//
// Converted from PostgreSQL 15's src/backend/access/gin/gininsert.c et al.
//
// GIN is an inverted index: it maps individual key elements (e.g. array
// members) to posting lists of heap TIDs. It excels at "contains" queries.
//
// pgcpp simplifications:
//   - Entry tree: a single flat page of (key, posting-block) entries
//   - Posting lists: pages of packed TIDs, chained via overflow
//   - Supports int32 element keys (for array containment)
//   - No VACUUM, no fast update, no pending list
//
// Page layout:
//   Block 0: meta page
//   Block 1: entry tree (flat page of GinEntryData items)
//   Block 2+: posting list pages (chained via btpo_next)

#include "access/gin.hpp"

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
using pgcpp::storage::BufferGetBlockNumber;
using pgcpp::storage::BufferGetPage;
using pgcpp::storage::ForkNumber;
using pgcpp::storage::Item;
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

// GIN page flags (stored in BTPageOpaqueData::btpo_flags).
constexpr uint32_t kGinMeta = 0x0100;     // meta page
constexpr uint32_t kGinEntry = 0x0200;    // entry tree page
constexpr uint32_t kGinPosting = 0x0400;  // posting list page

// GIN meta page magic.
constexpr uint32_t kGinMagic = 0x0682;

// Block numbers for the fixed entry tree page.
constexpr BlockNumber kGinEntryBlock = 1;

// GinEntryData — an entry in the entry tree page.
// Each entry maps a key element to the first posting list block.
struct GinEntryData {
    int32_t key = 0;          // the key element (int32 only for pgcpp)
    BlockNumber posting = 0;  // first posting list block (0 = none)
};

// GinMetaPageData — stored in the content area of block 0.
struct GinMetaPageData {
    uint32_t magic = 0;
    uint32_t nblocks = 0;  // total blocks
};

// Per-scan opaque state (stored in BTScanDescData::opaque).
struct GinScanOpaque {
    OffsetNumber entry_off = 0;           // current entry tree offset
    Buffer posting_buf = kInvalidBuffer;  // current posting list buffer
    OffsetNumber posting_off = 0;         // current posting list offset
    bool inited = false;
};

// --- Meta page helpers ---

void gin_init_meta_page(Page page) {
    PageInit(page, kBlckSz, kBTPageOpaqueSize);
    auto* opaque = _bt_page_getopaque(page);
    opaque->btpo_flags = kGinMeta;
    auto* meta = reinterpret_cast<GinMetaPageData*>(page + kPageHeaderSize);
    meta->magic = kGinMagic;
    meta->nblocks = 2;  // meta + entry tree
}

void gin_update_meta(Page page, uint32_t nblocks) {
    auto* meta = reinterpret_cast<GinMetaPageData*>(page + kPageHeaderSize);
    meta->nblocks = nblocks;
}

// --- Entry tree helpers ---

// Find an entry in the entry tree by key. Returns the offset (1-based) or
// kInvalidOffsetNumber if not found.
OffsetNumber gin_find_entry(Page page, int32_t key) {
    OffsetNumber max_off = PageGetMaxOffsetNumber(page);
    for (OffsetNumber off = 1; off <= max_off; off++) {
        auto* item_id = PageGetItemId(page, off);
        if (!ItemIdIsNormal(item_id))
            continue;
        auto* entry = reinterpret_cast<GinEntryData*>(PageGetItem(page, item_id));
        if (entry->key == key)
            return off;
    }
    return kInvalidOffsetNumber;
}

// Allocate a new posting list page.
Buffer gin_new_posting_page(Relation index) {
    index->rd_smgr = RelationGetSmgr(index);
    BlockNumber nblocks = RelationGetNumberOfBlocks(index);

    char pagebuf[kBlckSz];
    PageInit(pagebuf, kBlckSz, kBTPageOpaqueSize);
    auto* opaque = _bt_page_getopaque(pagebuf);
    opaque->btpo_flags = kGinPosting;
    opaque->btpo_next = 0;
    pgcpp::storage::smgrextend(index->rd_smgr, ForkNumber::kMain, nblocks, pagebuf, false);

    // Update meta nblocks.
    Buffer meta_buf = ReadBuffer(index->rd_smgr, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
    Page meta_page = BufferGetPage(meta_buf);
    gin_update_meta(meta_page, static_cast<uint32_t>(nblocks) + 1);
    MarkBufferDirty(meta_buf);
    ReleaseBuffer(meta_buf);

    return ReadBuffer(index->rd_smgr, ForkNumber::kMain, nblocks, ReadBufferMode::kNormal);
}

}  // namespace

// --- Index creation ---

void ginbuild(Relation index, [[maybe_unused]] BTKeyKind key_kind) {
    index->rd_smgr = RelationGetSmgr(index);

    // Block 0: meta page.
    char metabuf[kBlckSz];
    gin_init_meta_page(metabuf);
    pgcpp::storage::smgrextend(index->rd_smgr, ForkNumber::kMain, 0, metabuf, false);

    // Block 1: empty entry tree page.
    char entrybuf[kBlckSz];
    PageInit(entrybuf, kBlckSz, kBTPageOpaqueSize);
    auto* entry_opaque = _bt_page_getopaque(entrybuf);
    entry_opaque->btpo_flags = kGinEntry;
    entry_opaque->btpo_next = 0;
    pgcpp::storage::smgrextend(index->rd_smgr, ForkNumber::kMain, kGinEntryBlock, entrybuf, false);
}

// --- Insertion ---

bool gininsert(Relation index, BTKeyKind kind, const void* key, [[maybe_unused]] uint16_t key_len,
               const ItemPointerData& tid) {
    if (kind != BTKeyKind::kInt32) {
        // GIN in pgcpp only supports int32 keys.
        return false;
    }

    index->rd_smgr = RelationGetSmgr(index);
    int32_t key_val;
    std::memcpy(&key_val, key, sizeof(key_val));

    // Read the entry tree page.
    Buffer entry_buf =
        ReadBuffer(index->rd_smgr, ForkNumber::kMain, kGinEntryBlock, ReadBufferMode::kNormal);
    Page entry_page = BufferGetPage(entry_buf);

    // Find or create the entry for this key.
    OffsetNumber entry_off = gin_find_entry(entry_page, key_val);
    BlockNumber posting_block = 0;

    if (entry_off != kInvalidOffsetNumber) {
        // Entry exists — get its posting block.
        auto* item_id = PageGetItemId(entry_page, entry_off);
        auto* entry = reinterpret_cast<GinEntryData*>(PageGetItem(entry_page, item_id));
        posting_block = entry->posting;
    }

    if (posting_block == 0) {
        // No posting list yet — create one.
        Buffer posting_buf = gin_new_posting_page(index);
        posting_block = BufferGetBlockNumber(posting_buf);

        // Add the tid to the new posting page.
        Page posting_page = BufferGetPage(posting_buf);
        PageAddItem(posting_page, reinterpret_cast<Item>(const_cast<ItemPointerData*>(&tid)),
                    sizeof(ItemPointerData), kInvalidOffsetNumber, false);
        MarkBufferDirty(posting_buf);
        ReleaseBuffer(posting_buf);

        // Update or create the entry tree item.
        if (entry_off != kInvalidOffsetNumber) {
            auto* item_id = PageGetItemId(entry_page, entry_off);
            auto* entry = reinterpret_cast<GinEntryData*>(PageGetItem(entry_page, item_id));
            entry->posting = posting_block;
            MarkBufferDirty(entry_buf);
        } else {
            GinEntryData new_entry;
            new_entry.key = key_val;
            new_entry.posting = posting_block;
            OffsetNumber result = PageAddItem(entry_page, reinterpret_cast<Item>(&new_entry),
                                              sizeof(new_entry), kInvalidOffsetNumber, false);
            if (result == kInvalidOffsetNumber) {
                ReleaseBuffer(entry_buf);
                ereport(pgcpp::error::LogLevel::kWarning, "gininsert: entry tree page is full");
                return false;
            }
            MarkBufferDirty(entry_buf);
        }
        ReleaseBuffer(entry_buf);
        return true;
    }

    // Entry exists with a posting list — append the tid to the chain.
    ReleaseBuffer(entry_buf);

    Buffer buf =
        ReadBuffer(index->rd_smgr, ForkNumber::kMain, posting_block, ReadBufferMode::kNormal);
    Page page = BufferGetPage(buf);
    OffsetNumber result =
        PageAddItem(page, reinterpret_cast<Item>(const_cast<ItemPointerData*>(&tid)),
                    sizeof(ItemPointerData), kInvalidOffsetNumber, false);

    while (result == kInvalidOffsetNumber) {
        BTPageOpaque opaque = _bt_page_getopaque(page);
        BlockNumber next = opaque->btpo_next;

        if (next == 0) {
            // Create a new overflow posting page.
            MarkBufferDirty(buf);
            Buffer new_buf = gin_new_posting_page(index);
            Page curr_page = BufferGetPage(buf);
            BTPageOpaque curr_opaque = _bt_page_getopaque(curr_page);
            curr_opaque->btpo_next = BufferGetBlockNumber(new_buf);
            MarkBufferDirty(buf);
            ReleaseBuffer(buf);

            buf = new_buf;
            page = BufferGetPage(buf);
        } else {
            ReleaseBuffer(buf);
            buf = ReadBuffer(index->rd_smgr, ForkNumber::kMain, next, ReadBufferMode::kNormal);
            page = BufferGetPage(buf);
        }

        result = PageAddItem(page, reinterpret_cast<Item>(const_cast<ItemPointerData*>(&tid)),
                             sizeof(ItemPointerData), kInvalidOffsetNumber, false);
    }

    MarkBufferDirty(buf);
    ReleaseBuffer(buf);
    return true;
}

// --- Scan ---

BTScanDesc ginbeginscan(Relation index, BTKeyKind kind, const BTScanKeyData* scan_key) {
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

    auto* opaque = makePallocNode<GinScanOpaque>();
    scan->opaque = opaque;
    return scan;
}

bool gingettuple(BTScanDesc scan) {
    Relation index = scan->index;
    index->rd_smgr = RelationGetSmgr(index);
    auto* so = static_cast<GinScanOpaque*>(scan->opaque);
    if (so == nullptr)
        return false;

    if (!so->inited) {
        so->inited = true;
        so->entry_off = 1;
        so->posting_off = 1;
        so->posting_buf = kInvalidBuffer;

        // For equality scan, find the matching entry.
        if (scan->scan_key.strategy == BTStrategy::kEqual && scan->scan_key.key != nullptr) {
            Buffer entry_buf = ReadBuffer(index->rd_smgr, ForkNumber::kMain, kGinEntryBlock,
                                          ReadBufferMode::kNormal);
            Page entry_page = BufferGetPage(entry_buf);

            int32_t key_val;
            std::memcpy(&key_val, scan->scan_key.key, sizeof(key_val));
            OffsetNumber off = gin_find_entry(entry_page, key_val);

            if (off != kInvalidOffsetNumber) {
                auto* item_id = PageGetItemId(entry_page, off);
                auto* entry = reinterpret_cast<GinEntryData*>(PageGetItem(entry_page, item_id));
                if (entry->posting != 0) {
                    so->posting_buf = ReadBuffer(index->rd_smgr, ForkNumber::kMain, entry->posting,
                                                 ReadBufferMode::kNormal);
                    so->posting_off = 1;
                }
            }
            ReleaseBuffer(entry_buf);
        }
    }

    // Equality scan: drain the posting list of the matched entry.
    if (scan->scan_key.strategy == BTStrategy::kEqual && scan->scan_key.key != nullptr) {
        while (so->posting_buf != kInvalidBuffer) {
            Page page = BufferGetPage(so->posting_buf);
            OffsetNumber max_off = PageGetMaxOffsetNumber(page);

            while (so->posting_off <= max_off) {
                auto* item_id = PageGetItemId(page, so->posting_off);
                so->posting_off++;

                if (!ItemIdIsNormal(item_id))
                    continue;

                std::memcpy(&scan->curr_tid, PageGetItem(page, item_id), sizeof(ItemPointerData));
                return true;
            }

            BTPageOpaque opaque = _bt_page_getopaque(page);
            BlockNumber next = opaque->btpo_next;
            ReleaseBuffer(so->posting_buf);
            so->posting_buf = kInvalidBuffer;

            if (next != 0) {
                so->posting_buf =
                    ReadBuffer(index->rd_smgr, ForkNumber::kMain, next, ReadBufferMode::kNormal);
                so->posting_off = 1;
            }
        }
        return false;
    }

    // Full scan: walk all entries and their posting lists.
    Buffer entry_buf =
        ReadBuffer(index->rd_smgr, ForkNumber::kMain, kGinEntryBlock, ReadBufferMode::kNormal);
    Page entry_page = BufferGetPage(entry_buf);
    OffsetNumber entry_max = PageGetMaxOffsetNumber(entry_page);

    while (so->entry_off <= entry_max) {
        // If we don't have a posting buffer, try to open one for the current entry.
        if (so->posting_buf == kInvalidBuffer) {
            auto* item_id = PageGetItemId(entry_page, so->entry_off);
            if (ItemIdIsNormal(item_id)) {
                auto* entry = reinterpret_cast<GinEntryData*>(PageGetItem(entry_page, item_id));
                if (entry->posting != 0) {
                    so->posting_buf = ReadBuffer(index->rd_smgr, ForkNumber::kMain, entry->posting,
                                                 ReadBufferMode::kNormal);
                    so->posting_off = 1;
                }
            }
        }

        // Drain the current posting list.
        while (so->posting_buf != kInvalidBuffer) {
            Page page = BufferGetPage(so->posting_buf);
            OffsetNumber max_off = PageGetMaxOffsetNumber(page);

            while (so->posting_off <= max_off) {
                auto* item_id = PageGetItemId(page, so->posting_off);
                so->posting_off++;

                if (!ItemIdIsNormal(item_id))
                    continue;

                std::memcpy(&scan->curr_tid, PageGetItem(page, item_id), sizeof(ItemPointerData));
                ReleaseBuffer(entry_buf);
                return true;
            }

            BTPageOpaque opaque = _bt_page_getopaque(page);
            BlockNumber next = opaque->btpo_next;
            ReleaseBuffer(so->posting_buf);
            so->posting_buf = kInvalidBuffer;

            if (next != 0) {
                so->posting_buf =
                    ReadBuffer(index->rd_smgr, ForkNumber::kMain, next, ReadBufferMode::kNormal);
                so->posting_off = 1;
            }
        }

        // Move to the next entry.
        so->entry_off++;
    }

    ReleaseBuffer(entry_buf);
    return false;
}

void ginrescan(BTScanDesc scan, const BTScanKeyData* new_scan_key) {
    auto* so = static_cast<GinScanOpaque*>(scan->opaque);
    if (so != nullptr) {
        if (so->posting_buf != kInvalidBuffer) {
            ReleaseBuffer(so->posting_buf);
            so->posting_buf = kInvalidBuffer;
        }
        so->inited = false;
        so->entry_off = 0;
        so->posting_off = 0;
    }
    if (new_scan_key != nullptr) {
        scan->scan_key = *new_scan_key;
    }
}

void ginendscan(BTScanDesc scan) {
    if (scan == nullptr)
        return;

    auto* so = static_cast<GinScanOpaque*>(scan->opaque);
    if (so != nullptr) {
        if (so->posting_buf != kInvalidBuffer) {
            ReleaseBuffer(so->posting_buf);
            so->posting_buf = kInvalidBuffer;
        }
        destroyPallocNode(so);
    }
    destroyPallocNode(scan);
}

bool gincanreturn([[maybe_unused]] Relation index) {
    // GIN cannot return heap tuples (no amcanreturn in PostgreSQL).
    return false;
}

int64_t gingetbitmap(BTScanDesc scan, std::vector<ItemPointerData>* tids) {
    if (scan == nullptr || tids == nullptr)
        return 0;
    int64_t count = 0;
    while (gingettuple(scan)) {
        tids->push_back(scan->curr_tid);
        count++;
    }
    return count;
}

}  // namespace pgcpp::access

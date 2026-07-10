// brin.cpp — BRIN (Block Range Index) access method implementation.
//
// Converted from PostgreSQL 15's src/backend/access/brin/brin.c.
//
// BRIN stores a compact summary (min, max) for each range of heap pages.
// Scan skips ranges whose summary cannot satisfy the scan key, then returns
// candidate tids from the remaining ranges for residual checking.
//
// pgcpp simplifications:
//   - Summary per range: {first_block, min, max, count} (int32 keys only)
//   - Fixed pages_per_range (default 16)
//   - Stores the tids for each range explicitly (real BRIN returns pages)
//   - No VACUUM, no auto-summarize, no revmap pages
//
// Page layout:
//   Block 0: meta page (BrinMetaPageData in content area)
//   Block i (i >= 1): range page for range (i-1)
//     Item #1: BrinSummaryData (fixed-size summary header)
//     Items #2..: ItemPointerData (one per inserted tid)
//     Overflow pages chained via btpo_next when the range page fills up

#include "access/brin.hpp"

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

// BRIN page flags (stored in BTPageOpaqueData::btpo_flags).
constexpr uint32_t kBrinMeta = 0x0100;       // meta page
constexpr uint32_t kBrinRangePage = 0x0200;  // range summary page
constexpr uint32_t kBrinOverflow = 0x0400;   // overflow page (tid continuation)

// BRIN meta page magic.
constexpr uint32_t kBrinMagic = 0x0684;

// BRIN meta page data (stored in the content area of block 0).
struct BrinMetaPageData {
    uint32_t magic = 0;
    uint32_t pages_per_range = 0;
    uint32_t nblocks = 0;  // total blocks in the index
};

// BRIN range summary header (stored as item #1 on each range page).
struct BrinSummaryData {
    BlockNumber first_block = 0;  // first heap block in this range
    int32_t min = 0;              // minimum key value in this range
    int32_t max = 0;              // maximum key value in this range
    uint32_t count = 0;           // number of tids in this range
    bool has_data = false;        // false until first insert
};

// Per-scan opaque state.
struct BrinScanOpaque {
    BlockNumber curr_block = 0;  // current range page block being scanned
    Buffer currbuf = kInvalidBuffer;
    OffsetNumber curroff = 0;  // next offset to check
    bool inited = false;
};

// --- Meta page helpers ---

void brin_init_meta_page(Page page, uint32_t pages_per_range) {
    PageInit(page, kBlckSz, kBTPageOpaqueSize);
    auto* opaque = _bt_page_getopaque(page);
    opaque->btpo_flags = kBrinMeta;
    auto* meta = reinterpret_cast<BrinMetaPageData*>(page + kPageHeaderSize);
    meta->magic = kBrinMagic;
    meta->pages_per_range = pages_per_range;
    meta->nblocks = 1;  // just the meta page
}

BrinMetaPageData brin_get_meta(Page page) {
    BrinMetaPageData meta;
    std::memcpy(&meta, page + kPageHeaderSize, sizeof(meta));
    return meta;
}

void brin_update_meta_nblocks(Page page, uint32_t nblocks) {
    auto* meta = reinterpret_cast<BrinMetaPageData*>(page + kPageHeaderSize);
    meta->nblocks = nblocks;
}

// --- Range page helpers ---

// Create a new empty range page at the given block number.
void brin_create_range_page(Relation index, BlockNumber block_num,
                            [[maybe_unused]] BlockNumber first_block) {
    char pagebuf[kBlckSz];
    PageInit(pagebuf, kBlckSz, kBTPageOpaqueSize);
    auto* opaque = _bt_page_getopaque(pagebuf);
    opaque->btpo_flags = kBrinRangePage;
    opaque->btpo_prev = 0;
    opaque->btpo_next = 0;
    opaque->btpo_level = 0;
    index->rd_smgr = RelationGetSmgr(index);
    pgcpp::storage::smgrextend(index->rd_smgr, ForkNumber::kMain, block_num, pagebuf, false);
}

// Create an overflow page at the given block number.
void brin_create_overflow_page(Relation index, BlockNumber block_num) {
    char pagebuf[kBlckSz];
    PageInit(pagebuf, kBlckSz, kBTPageOpaqueSize);
    auto* opaque = _bt_page_getopaque(pagebuf);
    opaque->btpo_flags = kBrinOverflow;
    opaque->btpo_prev = 0;
    opaque->btpo_next = 0;
    opaque->btpo_level = 0;
    index->rd_smgr = RelationGetSmgr(index);
    pgcpp::storage::smgrextend(index->rd_smgr, ForkNumber::kMain, block_num, pagebuf, false);
}

// Ensure the range page for range_index exists. Extends the index as needed.
// Returns the block number of the range page.
BlockNumber brin_ensure_range_page(Relation index, uint32_t range_index) {
    index->rd_smgr = RelationGetSmgr(index);
    BlockNumber needed_block = static_cast<BlockNumber>(range_index + 1);

    Buffer meta_buf = ReadBuffer(index->rd_smgr, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
    Page meta_page = BufferGetPage(meta_buf);
    BrinMetaPageData meta = brin_get_meta(meta_page);

    if (meta.nblocks > needed_block) {
        // Range page already exists.
        ReleaseBuffer(meta_buf);
        return needed_block;
    }

    // Extend with empty pages up to needed_block (inclusive).
    for (BlockNumber b = meta.nblocks; b <= needed_block; b++) {
        brin_create_range_page(index, b, (b - 1) * meta.pages_per_range);
    }

    uint32_t new_nblocks = static_cast<uint32_t>(needed_block) + 1;
    brin_update_meta_nblocks(meta_page, new_nblocks);
    MarkBufferDirty(meta_buf);
    ReleaseBuffer(meta_buf);
    return needed_block;
}

// Read the summary header (item #1) from a range page. Returns has_data=false
// if the page has no items yet.
BrinSummaryData brin_get_summary(Page page) {
    BrinSummaryData summary;
    summary.has_data = false;
    OffsetNumber max_off = PageGetMaxOffsetNumber(page);
    if (max_off < 1)
        return summary;
    auto* item_id = PageGetItemId(page, 1);
    if (!ItemIdIsNormal(item_id))
        return summary;
    auto* s = reinterpret_cast<BrinSummaryData*>(PageGetItem(page, item_id));
    std::memcpy(&summary, s, sizeof(summary));
    return summary;
}

// Overwrite the summary header (item #1) in place. The summary item must
// already exist (same size, so we can overwrite directly).
void brin_overwrite_summary(Page page, const BrinSummaryData& summary) {
    auto* item_id = PageGetItemId(page, 1);
    if (!ItemIdIsNormal(item_id))
        return;
    auto* s = reinterpret_cast<BrinSummaryData*>(PageGetItem(page, item_id));
    std::memcpy(s, &summary, sizeof(summary));
}

// Check if a range's summary can satisfy the scan key.
// Returns true if the range MIGHT contain matching keys (candidate range).
bool brin_range_matches(const BrinSummaryData& summary, const BTScanKeyData& scan_key) {
    if (scan_key.strategy == BTStrategy::kAll || scan_key.key == nullptr)
        return true;

    int32_t scan_val;
    std::memcpy(&scan_val, scan_key.key, sizeof(scan_val));

    switch (scan_key.strategy) {
        case BTStrategy::kEqual:
            return scan_val >= summary.min && scan_val <= summary.max;
        case BTStrategy::kLess:
            return summary.min < scan_val;
        case BTStrategy::kLessEqual:
            return summary.min <= scan_val;
        case BTStrategy::kGreater:
            return summary.max > scan_val;
        case BTStrategy::kGreaterEqual:
            return summary.max >= scan_val;
        case BTStrategy::kAll:
            return true;
    }
    return false;
}

}  // namespace

// --- Index creation ---

void brinbuild(Relation index, [[maybe_unused]] BTKeyKind key_kind) {
    index->rd_smgr = RelationGetSmgr(index);

    // Block 0: meta page.
    char metabuf[kBlckSz];
    brin_init_meta_page(metabuf, kBrinDefaultPagesPerRange);
    pgcpp::storage::smgrextend(index->rd_smgr, ForkNumber::kMain, 0, metabuf, false);
    // Range pages are created lazily on insert.
}

// --- Insertion ---

bool brininsert(Relation index, BTKeyKind kind, const void* key, [[maybe_unused]] uint16_t key_len,
                const ItemPointerData& tid) {
    if (kind != BTKeyKind::kInt32) {
        ereport(pgcpp::error::LogLevel::kWarning, "brininsert: only int32 keys are supported");
        return false;
    }

    int32_t key_val;
    std::memcpy(&key_val, key, sizeof(key_val));

    index->rd_smgr = RelationGetSmgr(index);

    // Read meta for pages_per_range.
    Buffer meta_buf = ReadBuffer(index->rd_smgr, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
    Page meta_page = BufferGetPage(meta_buf);
    BrinMetaPageData meta = brin_get_meta(meta_page);
    ReleaseBuffer(meta_buf);

    uint32_t range_index = static_cast<uint32_t>(tid.ip_blkid) / meta.pages_per_range;
    BlockNumber range_block = brin_ensure_range_page(index, range_index);

    // Read the range page.
    Buffer buf =
        ReadBuffer(index->rd_smgr, ForkNumber::kMain, range_block, ReadBufferMode::kNormal);
    Page page = BufferGetPage(buf);
    BrinSummaryData summary = brin_get_summary(page);

    if (!summary.has_data) {
        // First insert for this range — create the summary item (#1).
        summary.first_block = range_index * meta.pages_per_range;
        summary.min = key_val;
        summary.max = key_val;
        summary.count = 0;
        summary.has_data = true;
        char sum_buf[sizeof(BrinSummaryData)];
        std::memcpy(sum_buf, &summary, sizeof(summary));
        PageAddItem(page, reinterpret_cast<Item>(sum_buf), sizeof(BrinSummaryData),
                    kInvalidOffsetNumber, false);
    }

    // Update the summary min/max and count.
    if (key_val < summary.min)
        summary.min = key_val;
    if (key_val > summary.max)
        summary.max = key_val;
    summary.count++;
    brin_overwrite_summary(page, summary);

    // Append the tid as item #2+ (or follow overflow chain).
    char tid_buf[sizeof(ItemPointerData)];
    std::memcpy(tid_buf, &tid, sizeof(tid));
    OffsetNumber result = PageAddItem(page, reinterpret_cast<Item>(tid_buf),
                                      sizeof(ItemPointerData), kInvalidOffsetNumber, false);

    while (result == kInvalidOffsetNumber) {
        // Page is full — follow or create overflow chain.
        BTPageOpaque opaque = _bt_page_getopaque(page);
        BlockNumber next_block = opaque->btpo_next;

        if (next_block == 0) {
            // Create a new overflow page.
            MarkBufferDirty(buf);
            index->rd_smgr = RelationGetSmgr(index);
            BlockNumber nblocks = RelationGetNumberOfBlocks(index);
            brin_create_overflow_page(index, nblocks);

            // Link the current page to the new overflow.
            BTPageOpaque curr_opaque = _bt_page_getopaque(page);
            curr_opaque->btpo_next = nblocks;
            MarkBufferDirty(buf);

            Buffer ovf_buf =
                ReadBuffer(index->rd_smgr, ForkNumber::kMain, nblocks, ReadBufferMode::kNormal);
            ReleaseBuffer(buf);
            buf = ovf_buf;
            page = BufferGetPage(buf);
        } else {
            ReleaseBuffer(buf);
            buf =
                ReadBuffer(index->rd_smgr, ForkNumber::kMain, next_block, ReadBufferMode::kNormal);
            page = BufferGetPage(buf);
        }

        result = PageAddItem(page, reinterpret_cast<Item>(tid_buf), sizeof(ItemPointerData),
                             kInvalidOffsetNumber, false);
    }

    MarkBufferDirty(buf);
    ReleaseBuffer(buf);
    return true;
}

// --- Scan ---

BTScanDesc brinbeginscan(Relation index, BTKeyKind kind, const BTScanKeyData* scan_key) {
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

    auto* opaque = makePallocNode<BrinScanOpaque>();
    scan->opaque = opaque;
    return scan;
}

bool bringettuple(BTScanDesc scan) {
    Relation index = scan->index;
    index->rd_smgr = RelationGetSmgr(index);
    auto* so = static_cast<BrinScanOpaque*>(scan->opaque);
    if (so == nullptr)
        return false;

    // Read meta for nblocks.
    Buffer meta_buf = ReadBuffer(index->rd_smgr, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
    Page meta_page = BufferGetPage(meta_buf);
    BrinMetaPageData meta = brin_get_meta(meta_page);
    ReleaseBuffer(meta_buf);

    if (!so->inited) {
        so->inited = true;
        so->curr_block = 1;  // first range page
        so->currbuf = kInvalidBuffer;
        so->curroff = 0;
    }

    while (so->curr_block < meta.nblocks) {
        // If we don't have a buffer pinned, read the current range page.
        if (so->currbuf == kInvalidBuffer) {
            so->currbuf = ReadBuffer(index->rd_smgr, ForkNumber::kMain, so->curr_block,
                                     ReadBufferMode::kNormal);
            so->curroff = 1;

            // Check if this range matches the scan key.
            Page page = BufferGetPage(so->currbuf);
            BrinSummaryData summary = brin_get_summary(page);
            if (!summary.has_data || !brin_range_matches(summary, scan->scan_key)) {
                // Skip this range.
                ReleaseBuffer(so->currbuf);
                so->currbuf = kInvalidBuffer;
                so->curr_block++;
                continue;
            }
        }

        // Walk items #2+ on the current page and overflow chain.
        Page page = BufferGetPage(so->currbuf);
        OffsetNumber max_off = PageGetMaxOffsetNumber(page);

        while (so->curroff <= max_off) {
            OffsetNumber off = so->curroff;
            so->curroff++;

            if (off == 1)
                continue;  // item #1 is the summary header

            auto* item_id = PageGetItemId(page, off);
            if (!ItemIdIsNormal(item_id))
                continue;

            auto* tid = reinterpret_cast<ItemPointerData*>(PageGetItem(page, item_id));
            scan->curr_tid = *tid;
            return true;
        }

        // Move to the overflow page if any.
        BTPageOpaque opaque = _bt_page_getopaque(page);
        BlockNumber next_block = opaque->btpo_next;

        ReleaseBuffer(so->currbuf);
        so->currbuf = kInvalidBuffer;

        if (next_block != 0) {
            so->currbuf =
                ReadBuffer(index->rd_smgr, ForkNumber::kMain, next_block, ReadBufferMode::kNormal);
            so->curroff = 1;
        } else {
            // Done with this range — move to the next.
            so->curr_block++;
        }
    }

    return false;
}

void brinrescan(BTScanDesc scan, const BTScanKeyData* new_scan_key) {
    auto* so = static_cast<BrinScanOpaque*>(scan->opaque);
    if (so != nullptr) {
        if (so->currbuf != kInvalidBuffer) {
            ReleaseBuffer(so->currbuf);
            so->currbuf = kInvalidBuffer;
        }
        so->inited = false;
        so->curr_block = 0;
        so->curroff = 0;
    }
    if (new_scan_key != nullptr) {
        scan->scan_key = *new_scan_key;
    }
}

void brinendscan(BTScanDesc scan) {
    if (scan == nullptr)
        return;

    auto* so = static_cast<BrinScanOpaque*>(scan->opaque);
    if (so != nullptr) {
        if (so->currbuf != kInvalidBuffer) {
            ReleaseBuffer(so->currbuf);
            so->currbuf = kInvalidBuffer;
        }
        destroyPallocNode(so);
    }
    destroyPallocNode(scan);
}

bool brincanreturn([[maybe_unused]] Relation index) {
    // BRIN cannot return heap tuples (lossy summarizing AM).
    return false;
}

int64_t bringetbitmap(BTScanDesc scan, std::vector<ItemPointerData>* tids) {
    if (scan == nullptr || tids == nullptr)
        return 0;
    int64_t count = 0;
    while (bringettuple(scan)) {
        tids->push_back(scan->curr_tid);
        count++;
    }
    return count;
}

}  // namespace pgcpp::access

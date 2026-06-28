// bufpage.cpp — Page layout and item manipulation.
//
// Converted from PostgreSQL 15's src/backend/storage/page/bufpage.c.
// Implements page initialization and item (tuple) insertion into pages.
// The page layout preserves PostgreSQL's design: a fixed header, a line
// pointer array growing downward, and tuple data growing upward from the
// end of the page.

#include "pgcpp/storage/bufpage.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/memory_context.hpp"

namespace mytoydb::storage {

// Maximum size for an item on a page. PostgreSQL limits this to 1/4 of the
// page to ensure efficient space utilization. We use the same heuristic.
static constexpr int kMaxHeapTupleSize = kBlckSz / 4;

// Alignment used for item storage on the page. Matches the alignment applied
// by PageAddItem (sizeof(ItemIdData) == 4 bytes).
static constexpr int kItemAlign = static_cast<int>(sizeof(ItemIdData));

// MaxAlignDown / MaxAlign — round `size` to a multiple of kItemAlign.
static inline int MaxAlignDown(int size) {
    return size & ~(kItemAlign - 1);
}
static inline int MaxAlignUp(int size) {
    return (size + (kItemAlign - 1)) & ~(kItemAlign - 1);
}

void PageInit(Page page, int page_size, int special_size) {
    auto* phdr = reinterpret_cast<PageHeader>(page);

    // Clear the entire page to zeros.
    std::memset(page, 0, page_size);

    // Set up the header.
    phdr->pd_lsn = 0;
    phdr->pd_checksum = 0;
    phdr->pd_flags = 0;
    phdr->pd_lower = kPageHeaderSize;
    // pd_upper starts at the end of the page, minus the special area.
    phdr->pd_upper = static_cast<LocationIndex>(page_size - special_size);
    phdr->pd_special = static_cast<LocationIndex>(page_size - special_size);
    phdr->pd_pagesize_version = kPageSizeVersion;
    phdr->pd_prune_xid = 0;
}

OffsetNumber PageAddItem(Page page, Item item, Size size, OffsetNumber offset_number,
                         bool is_heap) {
    auto* phdr = reinterpret_cast<PageHeader>(page);
    int lower = phdr->pd_lower;
    int upper = phdr->pd_upper;
    OffsetNumber line_off;

    if (offset_number == kInvalidOffsetNumber) {
        // Append: allocate the next line pointer at the end.
        line_off = PageGetMaxOffsetNumber(page) + 1;
    } else {
        // Use the specified offset (must be within existing range or the
        // next available slot).
        line_off = offset_number;
    }

    // Compute the position of the new line pointer.
    int item_id_index = line_off - 1;
    int new_lower = kPageHeaderSize + (item_id_index + 1) * sizeof(ItemIdData);

    // Check if the item fits.
    // The new item goes at pd_upper - size, and the line pointer array
    // extends to new_lower. There must be no overlap.
    int aligned_size = static_cast<int>(size);
    // Align to sizeof(ItemIdData) (4 bytes) for efficiency.
    aligned_size = (aligned_size + (sizeof(ItemIdData) - 1)) & ~(sizeof(ItemIdData) - 1);

    if (lower + aligned_size > upper || new_lower > upper - aligned_size) {
        // Not enough space.
        return kInvalidOffsetNumber;
    }

    // Place the item data at the top of the free space (growing down).
    int item_offset = upper - aligned_size;

    // Copy the item data into the page.
    if (item != nullptr && size > 0) {
        std::memcpy(page + item_offset, item, size);
    }

    // Set up the line pointer.
    ItemIdData* item_id = reinterpret_cast<ItemIdData*>(page + kPageHeaderSize) + item_id_index;
    item_id->li_off = static_cast<uint32_t>(item_offset);
    item_id->li_flags = kLPNormal;
    item_id->li_len = static_cast<uint32_t>(size);

    // Update the header.
    phdr->pd_lower = static_cast<LocationIndex>(new_lower);
    phdr->pd_upper = static_cast<LocationIndex>(item_offset);

    return line_off;
}

int PageGetHeapFreeSpace(Page page) {
    auto* phdr = reinterpret_cast<PageHeader>(page);
    int space = phdr->pd_upper - phdr->pd_lower;

    // Account for the line pointer we'd need to add.
    space -= sizeof(ItemIdData);

    // Align down to 4 bytes (sizeof(ItemIdData)).
    space &= ~(sizeof(ItemIdData) - 1);

    if (space < 0)
        space = 0;
    return space;
}

// --- M6 P0 extensions (Task 15.7.2) ---

int PageGetFreeSpace(Page page) {
    auto* phdr = reinterpret_cast<PageHeader>(page);
    int space = phdr->pd_upper - phdr->pd_lower;
    space -= sizeof(ItemIdData);
    space = MaxAlignDown(space);
    if (space < 0)
        space = 0;
    return space;
}

int PageGetExactFreeSpace(Page page) {
    auto* phdr = reinterpret_cast<PageHeader>(page);
    return phdr->pd_upper - phdr->pd_lower;
}

int PageGetFreeSpaceForMultipleTuples(Page page, int n) {
    auto* phdr = reinterpret_cast<PageHeader>(page);
    int space = phdr->pd_upper - phdr->pd_lower;
    space -= n * static_cast<int>(sizeof(ItemIdData));
    space = MaxAlignDown(space);
    if (space < 0)
        space = 0;
    return space;
}

// compactify_tuples — move all normal tuples to the end of the page in
// line-pointer order, packing them tightly. Used by PageRepairFragmentation
// and PageIndexMultiDelete. Tuples are processed from highest offset to
// lowest so that memmove never overwrites not-yet-moved data.
static void compactify_tuples(Page page) {
    auto* phdr = reinterpret_cast<PageHeader>(page);
    OffsetNumber nline = PageGetMaxOffsetNumber(page);

    // Collect normal line pointers with their current offsets.
    struct ItemEntry {
        OffsetNumber offset;     // line pointer index (1-based)
        LocationIndex item_off;  // current offset in page
        int aligned_len;         // MAXALIGN'd length
        int raw_len;             // actual tuple length
    };
    std::vector<ItemEntry> items;
    items.reserve(nline);
    for (OffsetNumber i = 1; i <= nline; ++i) {
        ItemIdData* lp = PageGetItemId(page, i);
        if (lp->li_flags == kLPNormal) {
            items.push_back({i, lp->li_off, MaxAlignUp(lp->li_len), lp->li_len});
        }
    }

    if (items.empty()) {
        phdr->pd_upper = phdr->pd_special;
        return;
    }

    // Sort by current offset descending: move the highest-offset tuple first
    // so it lands just below pd_special, then the next, and so on. This
    // guarantees no memmove source region is clobbered by a prior move.
    std::sort(items.begin(), items.end(),
              [](const ItemEntry& a, const ItemEntry& b) { return a.item_off > b.item_off; });

    LocationIndex new_upper = phdr->pd_special;
    for (const auto& item : items) {
        ItemIdData* lp = PageGetItemId(page, item.offset);
        new_upper -= static_cast<LocationIndex>(item.aligned_len);
        // Only move `raw_len` bytes; the padding (aligned_len - raw_len) is
        // garbage and need not be preserved.
        std::memmove(page + new_upper, page + item.item_off, item.raw_len);
        lp->li_off = new_upper;
    }
    phdr->pd_upper = new_upper;
}

void PageIndexTupleDelete(Page page, OffsetNumber offset) {
    auto* phdr = reinterpret_cast<PageHeader>(page);
    OffsetNumber nline = PageGetMaxOffsetNumber(page);

    if (offset < 1 || offset > nline) {
        ereport(mytoydb::error::LogLevel::kError, "PageIndexTupleDelete: invalid offset");
    }

    ItemIdData* lp = PageGetItemId(page, offset);
    if (lp->li_flags != kLPNormal) {
        ereport(mytoydb::error::LogLevel::kError,
                "PageIndexTupleDelete: line pointer is not normal");
    }

    // Save the deleted tuple's offset and aligned size before clearing.
    LocationIndex deleted_off = lp->li_off;
    int deleted_aligned = MaxAlignUp(lp->li_len);

    // Mark the line pointer as unused.
    lp->li_flags = kLPUnused;
    lp->li_off = 0;
    lp->li_len = 0;

    // If the deleted line pointer was the last one, shrink pd_lower to drop
    // trailing unused entries.
    if (offset == nline) {
        while (nline > 0 && PageGetItemId(page, nline)->li_flags == kLPUnused) {
            --nline;
        }
        phdr->pd_lower = static_cast<LocationIndex>(kPageHeaderSize + nline * sizeof(ItemIdData));
    }

    if (deleted_aligned == 0)
        return;

    // Close the gap left by the deleted tuple: move all normal tuples whose
    // offset is BELOW the deleted tuple's offset UP by `deleted_aligned`.
    // Process from highest offset to lowest to keep memmove sources valid.
    std::vector<ItemIdData*> to_move;
    to_move.reserve(nline);
    for (OffsetNumber i = 1; i <= PageGetMaxOffsetNumber(page); ++i) {
        ItemIdData* lpi = PageGetItemId(page, i);
        if (lpi->li_flags == kLPNormal && lpi->li_off < deleted_off) {
            to_move.push_back(lpi);
        }
    }
    std::sort(to_move.begin(), to_move.end(),
              [](ItemIdData* a, ItemIdData* b) { return a->li_off > b->li_off; });

    for (ItemIdData* lpi : to_move) {
        std::memmove(page + lpi->li_off + deleted_aligned, page + lpi->li_off, lpi->li_len);
        lpi->li_off += deleted_aligned;
    }

    phdr->pd_upper = static_cast<LocationIndex>(phdr->pd_upper + deleted_aligned);
}

void PageIndexMultiDelete(Page page, OffsetNumber* itemids, int nitems) {
    auto* phdr = reinterpret_cast<PageHeader>(page);
    OffsetNumber nline = PageGetMaxOffsetNumber(page);

    // Mark all specified line pointers as unused.
    for (int i = 0; i < nitems; ++i) {
        OffsetNumber off = itemids[i];
        if (off < 1 || off > nline)
            continue;
        ItemIdData* lp = PageGetItemId(page, off);
        lp->li_flags = kLPUnused;
        lp->li_off = 0;
        lp->li_len = 0;
    }

    // Shrink pd_lower if the tail of the line pointer array is now unused.
    while (nline > 0 && PageGetItemId(page, nline)->li_flags == kLPUnused) {
        --nline;
    }
    phdr->pd_lower = static_cast<LocationIndex>(kPageHeaderSize + nline * sizeof(ItemIdData));

    // Compact the tuple data.
    compactify_tuples(page);

    // Set the P_HAS_FREE_LINES flag (there may be unused slots in the middle).
    phdr->pd_flags |= kPageHasFreeLines;
}

void PageIndexTupleDeleteNoCompact(Page page, OffsetNumber offset) {
    auto* phdr = reinterpret_cast<PageHeader>(page);
    OffsetNumber nline = PageGetMaxOffsetNumber(page);

    if (offset < 1 || offset > nline)
        return;

    ItemIdData* lp = PageGetItemId(page, offset);
    lp->li_flags = kLPUnused;
    lp->li_off = 0;
    lp->li_len = 0;

    // If the deleted line pointer was the last one, shrink pd_lower.
    if (offset == nline) {
        while (nline > 0 && PageGetItemId(page, nline)->li_flags == kLPUnused) {
            --nline;
        }
        phdr->pd_lower = static_cast<LocationIndex>(kPageHeaderSize + nline * sizeof(ItemIdData));
    } else {
        // There's now a hole in the middle of the line pointer array.
        phdr->pd_flags |= kPageHasFreeLines;
    }
}

bool PageIndexTupleOverwrite(Page page, OffsetNumber offnum, Item newitem, Size newsize) {
    auto* phdr = reinterpret_cast<PageHeader>(page);
    OffsetNumber nline = PageGetMaxOffsetNumber(page);

    if (offnum < 1 || offnum > nline)
        return false;

    ItemIdData* lp = PageGetItemId(page, offnum);
    if (lp->li_flags != kLPNormal)
        return false;

    int old_raw = lp->li_len;
    int old_aligned = MaxAlignUp(old_raw);
    int new_aligned = MaxAlignUp(static_cast<int>(newsize));

    // Cannot grow in place.
    if (static_cast<int>(newsize) > old_raw)
        return false;

    // Copy the new item data over the old one (same offset).
    if (newitem != nullptr && newsize > 0) {
        std::memcpy(page + lp->li_off, newitem, newsize);
    }
    lp->li_len = static_cast<uint32_t>(newsize);

    // If the aligned size shrank, close the resulting gap by moving lower
    // tuples up (same compaction logic as PageIndexTupleDelete).
    if (new_aligned < old_aligned) {
        int diff = old_aligned - new_aligned;
        LocationIndex overwrite_off = lp->li_off;

        std::vector<ItemIdData*> to_move;
        to_move.reserve(nline);
        for (OffsetNumber i = 1; i <= nline; ++i) {
            ItemIdData* lpi = PageGetItemId(page, i);
            if (lpi->li_flags == kLPNormal && lpi != lp && lpi->li_off < overwrite_off) {
                to_move.push_back(lpi);
            }
        }
        std::sort(to_move.begin(), to_move.end(),
                  [](ItemIdData* a, ItemIdData* b) { return a->li_off > b->li_off; });

        for (ItemIdData* lpi : to_move) {
            std::memmove(page + lpi->li_off + diff, page + lpi->li_off, lpi->li_len);
            lpi->li_off += diff;
        }
        phdr->pd_upper = static_cast<LocationIndex>(phdr->pd_upper + diff);
    }

    return true;
}

void PageRepairFragmentation(Page page) {
    auto* phdr = reinterpret_cast<PageHeader>(page);
    OffsetNumber nline = PageGetMaxOffsetNumber(page);

    // Count normal items. If none, reset the page to empty.
    int nused = 0;
    for (OffsetNumber i = 1; i <= nline; ++i) {
        ItemIdData* lp = PageGetItemId(page, i);
        if (lp->li_flags == kLPNormal)
            ++nused;
    }

    if (nused == 0) {
        // No normal items: reset the content area.
        phdr->pd_upper = phdr->pd_special;
        phdr->pd_lower = static_cast<LocationIndex>(kPageHeaderSize);
        for (OffsetNumber i = 1; i <= nline; ++i) {
            ItemIdData* lp = PageGetItemId(page, i);
            lp->li_flags = kLPUnused;
            lp->li_off = 0;
            lp->li_len = 0;
        }
        phdr->pd_flags &= ~kPageHasFreeLines;
        return;
    }

    // Move all normal tuples to the end of the page, packed tightly.
    compactify_tuples(page);

    // Compact the line pointer array: shift normal items to the front,
    // clearing dead/redirect/unused slots at the back.
    OffsetNumber new_idx = 0;
    for (OffsetNumber i = 1; i <= nline; ++i) {
        ItemIdData* lp = PageGetItemId(page, i);
        if (lp->li_flags == kLPNormal) {
            ++new_idx;
            if (new_idx != i) {
                *PageGetItemId(page, new_idx) = *lp;
                lp->li_flags = kLPUnused;
                lp->li_off = 0;
                lp->li_len = 0;
            }
        } else {
            lp->li_flags = kLPUnused;
            lp->li_off = 0;
            lp->li_len = 0;
        }
    }
    phdr->pd_lower = static_cast<LocationIndex>(kPageHeaderSize + new_idx * sizeof(ItemIdData));

    // All line pointer slots are now normal (no holes) — clear the flag.
    phdr->pd_flags &= ~kPageHasFreeLines;
}

Page PageGetTempPage(Page page) {
    auto* phdr = reinterpret_cast<PageHeader>(page);
    int page_size = PageGetPageSize(page);
    int special_size = page_size - phdr->pd_special;

    Page temp = static_cast<Page>(mytoydb::memory::palloc(page_size));
    // Zero the new page, then copy the header and special area only.
    std::memset(temp, 0, page_size);
    std::memcpy(temp, page, kPageHeaderSize);
    if (special_size > 0) {
        std::memcpy(temp + phdr->pd_special, page + phdr->pd_special, special_size);
    }
    // Reset content pointers: empty line pointer array, empty tuple area.
    auto* thdr = reinterpret_cast<PageHeader>(temp);
    thdr->pd_lower = static_cast<LocationIndex>(kPageHeaderSize);
    thdr->pd_upper = phdr->pd_special;
    return temp;
}

Page PageGetTempPageCopy(Page page) {
    int page_size = PageGetPageSize(page);
    Page temp = static_cast<Page>(mytoydb::memory::palloc(page_size));
    std::memcpy(temp, page, page_size);
    return temp;
}

Page PageGetTempPageCopySpecial(Page page, int special_size) {
    auto* phdr = reinterpret_cast<PageHeader>(page);
    int page_size = PageGetPageSize(page);

    Page temp = static_cast<Page>(mytoydb::memory::palloc(page_size));
    std::memset(temp, 0, page_size);
    // Copy the header only.
    std::memcpy(temp, page, kPageHeaderSize);
    // Copy the special area to the end of the new page.
    if (special_size > 0) {
        int old_special_size = page_size - phdr->pd_special;
        int copy_size = std::min(old_special_size, special_size);
        std::memcpy(temp + page_size - special_size, page + phdr->pd_special, copy_size);
    }
    auto* thdr = reinterpret_cast<PageHeader>(temp);
    thdr->pd_lower = static_cast<LocationIndex>(kPageHeaderSize);
    thdr->pd_upper = static_cast<LocationIndex>(page_size - special_size);
    thdr->pd_special = static_cast<LocationIndex>(page_size - special_size);
    return temp;
}

void PageRestoreTempPage(Page temp_page, Page page) {
    int page_size = PageGetPageSize(temp_page);
    std::memcpy(page, temp_page, page_size);
    mytoydb::memory::pfree(temp_page);
}

}  // namespace mytoydb::storage

// bufpage.cpp — Page layout and item manipulation.
//
// Converted from PostgreSQL 15's src/backend/storage/page/bufpage.c.
// Implements page initialization and item (tuple) insertion into pages.
// The page layout preserves PostgreSQL's design: a fixed header, a line
// pointer array growing downward, and tuple data growing upward from the
// end of the page.

#include "mytoydb/storage/bufpage.h"

#include <cstring>

#include "mytoydb/common/error/elog.h"

namespace mytoydb::storage {

// Maximum size for an item on a page. PostgreSQL limits this to 1/4 of the
// page to ensure efficient space utilization. We use the same heuristic.
static constexpr int kMaxHeapTupleSize = kBlckSz / 4;

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

OffsetNumber PageAddItem(Page page, Item item, Size size,
                         OffsetNumber offset_number, bool is_heap) {
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
    ItemIdData* item_id = reinterpret_cast<ItemIdData*>(
        page + kPageHeaderSize) + item_id_index;
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

    if (space < 0) space = 0;
    return space;
}

}  // namespace mytoydb::storage

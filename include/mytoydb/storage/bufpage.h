// bufpage.h — Page layout and item manipulation.
//
// Converted from PostgreSQL 15's src/include/storage/bufpage.h.
//
// A page is the fundamental I/O unit of storage (8192 bytes by default).
// Layout:
//   +------------------+  offset 0
//   | PageHeaderData   |   24 bytes
//   +------------------+
//   | ItemIdData[]     |   line pointer array (grows downward →)
//   +------------------+
//   |   free space     |
//   +------------------+
//   | items (tuples)   |   (grows upward ←)
//   +------------------+
//   | special space    |   (for indexes; 0 for heap pages)
//   +------------------+  offset BLCKSZ
//
// The line pointer array starts immediately after the header and grows
// toward higher addresses. Tuple data grows from the end of the page
// toward lower addresses. Free space is between pd_lower and pd_upper.
#pragma once

#include <cstdint>
#include <cstddef>

#include "mytoydb/storage/block.h"
#include "mytoydb/transaction/transam.h"

namespace mytoydb::storage {

// LocationIndex — offset within a page (uint16_t in PostgreSQL).
using LocationIndex = uint16_t;

// OffsetNumber — index into the line pointer array (1-based for heap pages).
using OffsetNumber = uint16_t;

// InvalidOffsetNumber — sentinel for "no offset".
constexpr OffsetNumber kInvalidOffsetNumber = 0;
constexpr OffsetNumber kFirstOffsetNumber = 1;
constexpr OffsetNumber kMaxOffsetNumber = 0xFFFF;

// ItemId flags (li_flags field of ItemIdData).
constexpr uint16_t kLPUnused = 0;   // unused (should always be 0)
constexpr uint16_t kLPNormal = 1;   // used (should always be 1)
constexpr uint16_t kLPRedirect = 2; // HOT redirect (should be 2)
constexpr uint16_t kLPDead = 3;     // dead, may or may not have storage

// ItemIdData — a line pointer entry in the page header.
// Layout (32 bits total, matching PostgreSQL's bitfield):
//   li_off:   bits 0-14  (15 bits) — offset to tuple from page start
//   li_flags: bits 15-16 (2 bits)  — line pointer state
//   li_len:   bits 17-31 (15 bits) — tuple length
struct ItemIdData {
    uint32_t li_off : 15;
    uint32_t li_flags : 2;
    uint32_t li_len : 15;
};

// Size of the fixed page header (before line pointers).
constexpr int kPageHeaderSize = 24;

// Page layout version (encoded in pd_pagesize_version).
// PostgreSQL uses PD_PAGE_VERSION = 4, combined with page size as:
//   pd_pagesize_version = page_size | version
constexpr uint16_t kPageLayoutVersion = 4;
constexpr uint16_t kPageSizeVersion = static_cast<uint16_t>(kBlckSz) | kPageLayoutVersion;

// PageXLogRecPtr — LSN (Log Sequence Number) of the last WAL record
// affecting this page. In MyToyDB (no WAL yet), this is always 0.
using PageXLogRecPtr = uint64_t;

// TransactionId — re-exported from the transaction namespace for use in
// page headers (pd_prune_xid). PostgreSQL uses a global typedef; MyToyDB
// keeps it in mytoydb::transaction and aliases it here for convenience.
using TransactionId = mytoydb::transaction::TransactionId;

// PageHeaderData — the fixed-size header at the start of every page.
struct PageHeaderData {
    PageXLogRecPtr pd_lsn;           // 8 bytes — LSN: next byte after last xlog rec
    uint16_t pd_checksum;            // 2 bytes — checksum
    uint16_t pd_flags;               // 2 bytes — flag bits
    LocationIndex pd_lower;          // 2 bytes — offset to start of free space
    LocationIndex pd_upper;          // 2 bytes — offset to end of free space
    LocationIndex pd_special;        // 2 bytes — offset to start of special space
    uint16_t pd_pagesize_version;    // 2 bytes — page size and layout version
    TransactionId pd_prune_xid;      // 4 bytes — oldest prunable XID, or 0
    // ItemIdData pd_linp[] follows immediately after
};

// Page — pointer to a page's raw data (an array of bytes).
using Page = char*;

// PageHeader — pointer to a page's header (same as Page, but typed).
using PageHeader = PageHeaderData*;

// Item — pointer to item (tuple) data within a page.
using Item = char*;

// Size — size in bytes (PostgreSQL compatibility).
using Size = std::size_t;

// pd_flags bits
constexpr uint16_t kPageHasFreeLines = 0x0001;  // has unused line pointers?
constexpr uint16_t kPageFull = 0x0002;          // not enough free space for new item?
constexpr uint16_t kPageAllVisible = 0x0004;    // all tuples on page visible?
constexpr uint16_t kPageChecksumValid = 0x0008; // checksum valid on page?

// --- Page access macros (converted to inline functions) -----

// PageGetItemId — return pointer to the specified line pointer.
inline ItemIdData* PageGetItemId(Page page, OffsetNumber offset_number) {
    return reinterpret_cast<ItemIdData*>(
        page + kPageHeaderSize) + (offset_number - 1);
}

// PageGetMaxOffsetNumber — return the maximum offset number on the page.
// This is the number of line pointers currently allocated.
inline OffsetNumber PageGetMaxOffsetNumber(Page page) {
    auto* phdr = reinterpret_cast<PageHeader>(page);
    return static_cast<OffsetNumber>(
        (phdr->pd_lower - kPageHeaderSize) / sizeof(ItemIdData));
}

// PageGetItem — return pointer to item data referenced by a line pointer.
inline Item PageGetItem(Page page, ItemIdData* item_id) {
    return page + item_id->li_off;
}

// ItemIdGetOffset — extract offset from a line pointer.
inline LocationIndex ItemIdGetOffset(const ItemIdData* item_id) {
    return item_id->li_off;
}

// ItemIdGetLength — extract length from a line pointer.
inline uint16_t ItemIdGetLength(const ItemIdData* item_id) {
    return item_id->li_len;
}

// ItemIdGetFlags — extract flags from a line pointer.
inline uint16_t ItemIdGetFlags(const ItemIdData* item_id) {
    return item_id->li_flags;
}

// ItemIdIsUsed — true if the line pointer is in use (not unused).
inline bool ItemIdIsUsed(const ItemIdData* item_id) {
    return item_id->li_flags != kLPUnused;
}

// ItemIdIsNormal — true if the line pointer is normal (has storage).
inline bool ItemIdIsNormal(const ItemIdData* item_id) {
    return item_id->li_flags == kLPNormal;
}

// ItemIdIsDead — true if the line pointer is dead.
inline bool ItemIdIsDead(const ItemIdData* item_id) {
    return item_id->li_flags == kLPDead;
}

// PageGetPageSize — extract page size from pd_pagesize_version.
inline int PageGetPageSize(Page page) {
    auto* phdr = reinterpret_cast<PageHeader>(page);
    return phdr->pd_pagesize_version & static_cast<uint16_t>(~kPageLayoutVersion);
}

// PageGetPageLayoutVersion — extract layout version from pd_pagesize_version.
inline int PageGetPageLayoutVersion(Page page) {
    auto* phdr = reinterpret_cast<PageHeader>(page);
    return phdr->pd_pagesize_version & kPageLayoutVersion;
}

// PageGetSpecialPointer — return pointer to the special area.
inline char* PageGetSpecialPointer(Page page) {
    auto* phdr = reinterpret_cast<PageHeader>(page);
    return page + phdr->pd_special;
}

// PageGetContents — return pointer to the start of the content area
// (immediately after the header, where line pointers begin).
inline char* PageGetContents(Page page) {
    return page + kPageHeaderSize;
}

// --- Page operations (implemented in bufpage.cpp) ---

// PageInit — initialize a page to the empty state.
// Sets up the header, clears the line pointer array, and reserves the
// special area at the end of the page.
void PageInit(Page page, int page_size, int special_size);

// PageAddItem — add an item to a page.
// Returns the offset number assigned to the item, or kInvalidOffsetNumber
// on failure (page full). If offset_number is kInvalidOffsetNumber, the
// item is appended to the end of the line pointer array.
OffsetNumber PageAddItem(Page page, Item item, Size size,
                         OffsetNumber offset_number, bool is_heap);

// PageGetHeapFreeSpace — return the number of free bytes available for
// a new heap item (accounting for line pointer and alignment).
int PageGetHeapFreeSpace(Page page);

}  // namespace mytoydb::storage

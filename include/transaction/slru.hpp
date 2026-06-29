// slru.h — Simple LRU shared buffer for CLOG / commit_ts / multixact pages.
//
// Converted from PostgreSQL 15's src/include/access/slru.h.
//
// SLRU (Simple LRU) is a fixed-size page cache used for transaction-status
// data that must be shared but doesn't go through the main buffer manager.
// In PostgreSQL, CLOG, commit timestamps, multixact offsets/members, and
// replication slot data all use SLRU.
//
// pgcpp keeps the API but uses an in-memory std::vector<Page> for simplicity.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pgcpp::transaction {

// SLRU_PAGE_SIZE — matches PostgreSQL's BLCKSZ (8 KB).
constexpr int kSlruPageSize = 8192;

// SlruPageStatus — state of a page in the SLRU cache.
enum class SlruPageStatus {
    kEmpty,  // no data loaded
    kValid,  // valid data
    kDirty,  // modified, not yet flushed
};

// SlruPage — one page in the SLRU cache.
struct SlruPage {
    int pageno = 0;  // page number (offset / PAGE_SIZE)
    SlruPageStatus status = SlruPageStatus::kEmpty;
    std::vector<uint8_t> data;  // kSlruPageSize bytes

    SlruPage() : data(kSlruPageSize, 0) {}
};

// SlruCtl — control block for an SLRU instance (e.g., "clog", "commit_ts").
struct SlruCtl {
    std::string name;             // for diagnostics
    std::vector<SlruPage> pages;  // the cache (fixed capacity)
    std::size_t capacity = 16;    // max pages in cache

    // Statistics
    uint64_t reads = 0;
    uint64_t writes = 0;
    uint64_t flushes = 0;
};

// SimpleLruInit — create an SLRU with the given name and capacity.
SlruCtl* SimpleLruInit(const std::string& name, std::size_t capacity = 16);

// SimpleLruRead — read `len` bytes at the given page offset. If the page
// is not in cache, it is "loaded" (in pgcpp, initialized to zeros since
// there is no on-disk backing store). Copies data into `dst`.
void SimpleLruRead(SlruCtl* ctl, int pageno, int offset, void* dst, std::size_t len);

// SimpleLruWrite — write `len` bytes at the given page offset. The page is
// marked dirty. If the page is not in cache, it is loaded first.
void SimpleLruWrite(SlruCtl* ctl, int pageno, int offset, const void* src, std::size_t len);

// SimpleLruFlush — write all dirty pages back to "disk" (no-op in pgcpp:
// the in-memory cache is the store). Marks pages as valid.
void SimpleLruFlush(SlruCtl* ctl);

// SimpleLruReset — clear all pages and statistics.
void SimpleLruReset(SlruCtl* ctl);

// SimpleLruFree — destroy an SLRU instance.
void SimpleLruFree(SlruCtl* ctl);

}  // namespace pgcpp::transaction

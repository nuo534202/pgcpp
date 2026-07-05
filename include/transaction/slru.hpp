// slru.h — Simple LRU shared buffer for CLOG / commit_ts / multixact pages.
//
// Converted from PostgreSQL 15's src/include/access/slru.h.
//
// SLRU (Simple LRU) is a fixed-size page cache used for transaction-status
// data that must be shared but doesn't go through the main buffer manager.
// In PostgreSQL, CLOG, commit timestamps, multixact offsets/members, and
// replication slot data all use SLRU.
//
// pgcpp keeps the API and adds optional disk persistence: when a disk_dir
// is configured via SimpleLruInit, pages are loaded from and flushed to
// files named <disk_dir>/<hex_pageno>. Eviction writes dirty pages back
// to disk. Flush writes all dirty pages and fsyncs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

namespace pgcpp::transaction {

// SLRU_PAGE_SIZE — matches PostgreSQL's BLCKSZ (8 KB).
constexpr int kSlruPageSize = 8192;

// SlruPageStatus — state of a page in the SLRU cache.
enum class SlruPageStatus {
    kEmpty,  // no data loaded
    kValid,  // valid data (clean)
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
    std::string name;           // for diagnostics
    std::size_t capacity = 16;  // max pages in cache

    // Disk persistence: if disk_dir is non-empty, pages are loaded from and
    // flushed to <disk_dir>/<hex_pageno>. Empty means in-memory only.
    std::string disk_dir;

    // Page cache: indexed by pageno for O(1) lookup. The LRU order is
    // maintained by lru_order (most-recently-used at the back).
    std::unordered_map<int, SlruPage> pages;
    std::list<int> lru_order;  // pageno values, MRU at back

    // Statistics
    uint64_t reads = 0;
    uint64_t writes = 0;
    uint64_t flushes = 0;
};

// SimpleLruInit — create an SLRU with the given name and capacity.
// If disk_dir is non-empty, pages are persisted to that directory.
SlruCtl* SimpleLruInit(const std::string& name, std::size_t capacity = 16,
                       const std::string& disk_dir = "");

// SimpleLruRead — read `len` bytes at the given page offset. If the page
// is not in cache, it is loaded from disk (or zero-initialized if the
// file doesn't exist). Copies data into `dst`.
void SimpleLruRead(SlruCtl* ctl, int pageno, int offset, void* dst, std::size_t len);

// SimpleLruWrite — write `len` bytes at the given page offset. The page
// is marked dirty. If the page is not in cache, it is loaded first.
void SimpleLruWrite(SlruCtl* ctl, int pageno, int offset, const void* src, std::size_t len);

// SimpleLruFlush — write all dirty pages back to disk and fsync.
// Marks pages as valid (clean). No-op if no disk_dir is configured.
void SimpleLruFlush(SlruCtl* ctl);

// SimpleLruReset — clear all pages and statistics (does NOT delete disk files).
void SimpleLruReset(SlruCtl* ctl);

// SimpleLruFree — destroy an SLRU instance (flushes dirty pages first).
void SimpleLruFree(SlruCtl* ctl);

}  // namespace pgcpp::transaction

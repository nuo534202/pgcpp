// slru.cpp — Simple LRU shared buffer for CLOG / commit_ts / multixact pages.
//
// Converted from PostgreSQL 15's src/backend/access/transam/slru.cpp.
//
// SLRU (Simple LRU) is a fixed-size page cache used for transaction-status
// data that must be shared but doesn't go through the main buffer manager.
// pgcpp implements true LRU eviction (via a list) and optional disk
// persistence: when disk_dir is set, pages are loaded from / flushed to
// <disk_dir>/<hex_pageno>. Dirty pages are written back on eviction and
// on SimpleLruFlush (with fsync).
#include "transaction/slru.hpp"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace pgcpp::transaction {

namespace {

// WriteAllToFd — write exactly `len` bytes to `fd`, retrying on EINTR.
bool WriteAllToFd(int fd, const uint8_t* data, std::size_t len) {
    std::size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, data + written, len - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        written += static_cast<std::size_t>(n);
    }
    return true;
}

// PageFilePath — path for a page's on-disk file: <disk_dir>/<hex_pageno>.
std::string PageFilePath(const std::string& dir, int pageno) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04X", pageno);
    return dir + "/" + buf;
}

// ReadPageFromDisk — read a page from disk into `page->data`. Returns
// true on success, false if the file doesn't exist or is short.
bool ReadPageFromDisk(const std::string& dir, int pageno, SlruPage* page) {
    std::string path = PageFilePath(dir, pageno);
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    ssize_t n = read(fd, page->data.data(), kSlruPageSize);
    close(fd);
    if (n < 0) return false;
    // If the file is short, the remaining bytes stay zero (page->data was
    // zero-initialized by the constructor). This matches PG behavior.
    return true;
}

// WritePageToDisk — write a page to disk and fsync.
void WritePageToDisk(const std::string& dir, int pageno, const SlruPage* page) {
    // Create directory if it doesn't exist.
    mkdir(dir.c_str(), 0700);
    std::string path = PageFilePath(dir, pageno);
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (fd < 0) return;
    WriteAllToFd(fd, page->data.data(), kSlruPageSize);
    fsync(fd);
    close(fd);
}

// TouchPage — move `pageno` to the MRU end of the LRU list.
void TouchPage(SlruCtl* ctl, int pageno) {
    for (auto it = ctl->lru_order.begin(); it != ctl->lru_order.end(); ++it) {
        if (*it == pageno) {
            ctl->lru_order.erase(it);
            break;
        }
    }
    ctl->lru_order.push_back(pageno);
}

// EvictOnePage — evict the least-recently-used page. If it's dirty, write
// it to disk first. No-op if the cache is empty.
void EvictOnePage(SlruCtl* ctl) {
    if (ctl->lru_order.empty()) return;
    int victim = ctl->lru_order.front();
    ctl->lru_order.pop_front();

    auto it = ctl->pages.find(victim);
    if (it == ctl->pages.end()) return;

    if (it->second.status == SlruPageStatus::kDirty && !ctl->disk_dir.empty()) {
        WritePageToDisk(ctl->disk_dir, victim, &it->second);
    }
    ctl->pages.erase(it);
}

// LoadPage — ensure the page with `pageno` is in the cache. If the page
// is not present, it is loaded from disk (or zero-initialized if the file
// doesn't exist). Evicts an LRU page if the cache is full. The page is
// moved to the MRU end of the LRU list.
SlruPage* LoadPage(SlruCtl* ctl, int pageno) {
    auto it = ctl->pages.find(pageno);
    if (it != ctl->pages.end()) {
        TouchPage(ctl, pageno);
        return &it->second;
    }

    // Cache miss: evict if full.
    if (ctl->pages.size() >= ctl->capacity) {
        EvictOnePage(ctl);
    }

    // Create a new page entry.
    SlruPage page;
    page.pageno = pageno;
    page.status = SlruPageStatus::kValid;

    // Try to load from disk.
    if (!ctl->disk_dir.empty()) {
        ReadPageFromDisk(ctl->disk_dir, pageno, &page);
        // If the file doesn't exist, page.data stays zero-initialized.
    }

    auto result = ctl->pages.emplace(pageno, std::move(page));
    ctl->lru_order.push_back(pageno);
    return &result.first->second;
}

}  // namespace

SlruCtl* SimpleLruInit(const std::string& name, std::size_t capacity,
                        const std::string& disk_dir) {
    SlruCtl* ctl = new SlruCtl();
    ctl->name = name;
    ctl->capacity = capacity;
    ctl->disk_dir = disk_dir;
    return ctl;
}

void SimpleLruRead(SlruCtl* ctl, int pageno, int offset, void* dst, std::size_t len) {
    SlruPage* page = LoadPage(ctl, pageno);
    ++ctl->reads;
    std::memcpy(dst, page->data.data() + offset, len);
}

void SimpleLruWrite(SlruCtl* ctl, int pageno, int offset, const void* src, std::size_t len) {
    SlruPage* page = LoadPage(ctl, pageno);
    ++ctl->writes;
    std::memcpy(page->data.data() + offset, src, len);
    page->status = SlruPageStatus::kDirty;
}

void SimpleLruFlush(SlruCtl* ctl) {
    if (!ctl->disk_dir.empty()) {
        mkdir(ctl->disk_dir.c_str(), 0700);
        for (auto& [pageno, page] : ctl->pages) {
            if (page.status == SlruPageStatus::kDirty) {
                WritePageToDisk(ctl->disk_dir, pageno, &page);
                page.status = SlruPageStatus::kValid;
            }
        }
    } else {
        // No disk backing: just clear dirty flags.
        for (auto& [pageno, page] : ctl->pages) {
            page.status = SlruPageStatus::kValid;
        }
    }
    ++ctl->flushes;
}

void SimpleLruReset(SlruCtl* ctl) {
    ctl->pages.clear();
    ctl->lru_order.clear();
    ctl->reads = 0;
    ctl->writes = 0;
    ctl->flushes = 0;
}

void SimpleLruFree(SlruCtl* ctl) {
    if (ctl != nullptr) {
        SimpleLruFlush(ctl);
        delete ctl;
    }
}

}  // namespace pgcpp::transaction

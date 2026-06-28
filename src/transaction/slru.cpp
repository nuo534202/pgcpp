// slru.cpp — Simple LRU shared buffer for CLOG / commit_ts / multixact pages.
//
// Converted from PostgreSQL 15's src/backend/access/transam/slru.cpp.
//
// SLRU (Simple LRU) is a fixed-size page cache used for transaction-status
// data that must be shared but doesn't go through the main buffer manager.
// MyToyDB keeps the API but uses an in-memory std::vector<Page> for simplicity;
// the in-memory store IS the backing store, so flush is a no-op that just
// clears dirty flags. Eviction is FIFO (the page at index 0), not true LRU.
#include "mytoydb/transaction/slru.hpp"

#include <cstring>
#include <vector>

namespace mytoydb::transaction {

namespace {

// FindPage — return a pointer to the cached page with `pageno`, or nullptr.
SlruPage* FindPage(SlruCtl* ctl, int pageno) {
    for (SlruPage& page : ctl->pages) {
        if (page.pageno == pageno) {
            return &page;
        }
    }
    return nullptr;
}

// LoadPage — ensure the page with `pageno` is in the cache. If the page is
// not present and the cache is full, evict the page at index 0 (FIFO; we do
// not implement true LRU). The new page is initialized to zeros with status
// kValid. Returns a pointer to the (possibly newly loaded) page.
SlruPage* LoadPage(SlruCtl* ctl, int pageno) {
    if (SlruPage* page = FindPage(ctl, pageno)) {
        return page;
    }
    if (ctl->pages.size() >= ctl->capacity) {
        ctl->pages.erase(ctl->pages.begin());
    }
    SlruPage page;
    page.pageno = pageno;
    page.status = SlruPageStatus::kValid;
    // page.data is zero-initialized by the SlruPage default constructor.
    ctl->pages.push_back(std::move(page));
    return &ctl->pages.back();
}

}  // namespace

SlruCtl* SimpleLruInit(const std::string& name, std::size_t capacity) {
    SlruCtl* ctl = new SlruCtl();
    ctl->name = name;
    ctl->capacity = capacity;
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
    for (SlruPage& page : ctl->pages) {
        page.status = SlruPageStatus::kValid;
    }
    ++ctl->flushes;
}

void SimpleLruReset(SlruCtl* ctl) {
    ctl->pages.clear();
    ctl->reads = 0;
    ctl->writes = 0;
    ctl->flushes = 0;
}

void SimpleLruFree(SlruCtl* ctl) {
    delete ctl;
}

}  // namespace mytoydb::transaction

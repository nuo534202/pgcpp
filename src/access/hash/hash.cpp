// hash.cpp — Hash index access method implementation.
//
// Converted from PostgreSQL 15's src/backend/access/hash/hash.c.
//
// The hash AM maps keys to bucket pages via a hash function. It supports
// only equality lookups (point queries). Each bucket page holds (key, tid)
// entries; overflow pages chain from full buckets.
//
// pgcpp simplifications:
//   - Fixed bucket count (no split / linear hashing)
//   - Supports int32, int64, and text keys
//   - Reuses BTItemData entry format and _bt_compare_keys for key comparison
//   - No VACUUM, no page deletion
//
// Page layout:
//   Block 0: meta page (HashMetaPageData in content area)
//   Blocks 1..nbuckets: primary bucket pages
//   Overflow pages: chained from bucket pages via btpo_next

#include "access/hash.hpp"

#include <cstring>
#include <string>
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
using pgcpp::storage::ItemIdGetLength;
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

// Hash page flags (stored in BTPageOpaqueData::btpo_flags).
constexpr uint32_t kHashMeta = 0x0100;      // meta page
constexpr uint32_t kHashBucket = 0x0200;    // primary bucket page
constexpr uint32_t kHashOverflow = 0x0400;  // overflow page

// Hash meta page magic.
constexpr uint32_t kHashMagic = 0x0681;

// Default number of buckets. pgcpp uses a fixed bucket count (no splitting).
constexpr uint32_t kHashDefaultBuckets = 16;

// Hash meta page data (stored in the content area of block 0).
struct HashMetaPageData {
    uint32_t magic = 0;
    uint32_t nbuckets = 0;
    uint32_t nblocks = 0;  // total blocks (meta + buckets + overflows)
};

// Per-scan opaque state (stored in BTScanDescData::opaque).
struct HashScanOpaque {
    uint32_t bucket = 0;              // current bucket being scanned
    Buffer currbuf = kInvalidBuffer;  // current buffer (pinned)
    OffsetNumber curroff = 0;         // next offset to check
    bool inited = false;              // has the scan been positioned?
};

// --- Meta page helpers ---

void hash_init_meta_page(Page page, uint32_t nbuckets) {
    PageInit(page, kBlckSz, kBTPageOpaqueSize);
    auto* opaque = _bt_page_getopaque(page);
    opaque->btpo_flags = kHashMeta;
    auto* meta = reinterpret_cast<HashMetaPageData*>(page + kPageHeaderSize);
    meta->magic = kHashMagic;
    meta->nbuckets = nbuckets;
    meta->nblocks = nbuckets + 1;  // meta + bucket pages
}

HashMetaPageData hash_get_meta(Page page) {
    HashMetaPageData meta;
    std::memcpy(&meta, page + kPageHeaderSize, sizeof(meta));
    return meta;
}

void hash_update_meta(Page page, uint32_t nblocks) {
    auto* meta = reinterpret_cast<HashMetaPageData*>(page + kPageHeaderSize);
    meta->nblocks = nblocks;
}

// --- Bucket page helpers ---

Buffer hash_create_page(Relation index, BlockNumber block_num, uint32_t flags,
                        uint32_t bucket_num) {
    char pagebuf[kBlckSz];
    PageInit(pagebuf, kBlckSz, kBTPageOpaqueSize);
    auto* opaque = _bt_page_getopaque(pagebuf);
    opaque->btpo_flags = flags;
    opaque->btpo_prev = bucket_num;  // store bucket number in btpo_prev
    opaque->btpo_next = 0;           // no overflow yet
    opaque->btpo_level = 0;
    index->rd_smgr = RelationGetSmgr(index);
    pgcpp::storage::smgrextend(index->rd_smgr, ForkNumber::kMain, block_num, pagebuf, false);
    return ReadBuffer(index->rd_smgr, ForkNumber::kMain, block_num, ReadBufferMode::kNormal);
}

// Find the primary bucket page for a given bucket number.
// Bucket 0 is at block 1, bucket 1 at block 2, etc.
BlockNumber hash_bucket_to_block(uint32_t bucket) {
    return static_cast<BlockNumber>(bucket + 1);
}

// Allocate a new overflow page at the end of the relation.
Buffer hash_add_overflow_page(Relation index, uint32_t bucket_num) {
    index->rd_smgr = RelationGetSmgr(index);
    BlockNumber nblocks = RelationGetNumberOfBlocks(index);
    Buffer buf = hash_create_page(index, nblocks, kHashOverflow, bucket_num);

    // Update meta page's nblocks.
    Buffer meta_buf = ReadBuffer(index->rd_smgr, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
    Page meta_page = BufferGetPage(meta_buf);
    hash_update_meta(meta_page, static_cast<uint32_t>(nblocks) + 1);
    MarkBufferDirty(meta_buf);
    ReleaseBuffer(meta_buf);

    return buf;
}

}  // namespace

// --- hashcalc — the hash function ---

uint32_t hashcalc(BTKeyKind kind, const void* key, uint16_t key_len, uint32_t nbuckets) {
    uint32_t h = 0;
    if (kind == BTKeyKind::kInt32) {
        int32_t v;
        std::memcpy(&v, key, sizeof(v));
        h = static_cast<uint32_t>(v);
    } else if (kind == BTKeyKind::kInt64) {
        int64_t v;
        std::memcpy(&v, key, sizeof(v));
        h = static_cast<uint32_t>(v ^ (v >> 32));
    } else {
        // text: simple hash
        const auto* s = static_cast<const char*>(key);
        for (uint16_t i = 0; i < key_len; i++) {
            h = h * 31 + static_cast<unsigned char>(s[i]);
        }
    }
    return h % nbuckets;
}

// --- Index creation ---

void hashbuild(Relation index, [[maybe_unused]] BTKeyKind key_kind) {
    index->rd_smgr = RelationGetSmgr(index);

    // Block 0: meta page.
    char metabuf[kBlckSz];
    hash_init_meta_page(metabuf, kHashDefaultBuckets);
    pgcpp::storage::smgrextend(index->rd_smgr, ForkNumber::kMain, 0, metabuf, false);

    // Blocks 1..nbuckets: empty bucket pages.
    for (uint32_t b = 0; b < kHashDefaultBuckets; b++) {
        char pagebuf[kBlckSz];
        PageInit(pagebuf, kBlckSz, kBTPageOpaqueSize);
        auto* opaque = _bt_page_getopaque(pagebuf);
        opaque->btpo_flags = kHashBucket;
        opaque->btpo_prev = b;  // bucket number
        opaque->btpo_next = 0;
        opaque->btpo_level = 0;
        pgcpp::storage::smgrextend(index->rd_smgr, ForkNumber::kMain, hash_bucket_to_block(b),
                                   pagebuf, false);
    }
}

// --- Insertion ---

bool hashinsert(Relation index, BTKeyKind kind, const void* key, uint16_t key_len,
                const ItemPointerData& tid) {
    index->rd_smgr = RelationGetSmgr(index);

    // Read meta page for bucket count.
    Buffer meta_buf = ReadBuffer(index->rd_smgr, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
    Page meta_page = BufferGetPage(meta_buf);
    HashMetaPageData meta = hash_get_meta(meta_page);
    ReleaseBuffer(meta_buf);

    uint32_t bucket = hashcalc(kind, key, key_len, meta.nbuckets);
    BlockNumber block = hash_bucket_to_block(bucket);

    // Build the index entry (same format as btree: tid + key).
    uint16_t item_size = _bt_item_size(kind, key, key_len);
    char item_buf[256];
    _bt_build_item(reinterpret_cast<BTItem>(item_buf), kind, key, key_len, tid);

    // Walk the bucket's overflow chain looking for a page with space.
    Buffer buf = ReadBuffer(index->rd_smgr, ForkNumber::kMain, block, ReadBufferMode::kNormal);
    Page page = BufferGetPage(buf);
    OffsetNumber result =
        PageAddItem(page, reinterpret_cast<Item>(item_buf), item_size, kInvalidOffsetNumber, false);

    while (result == kInvalidOffsetNumber) {
        // Page is full — follow or create overflow chain.
        BTPageOpaque opaque = _bt_page_getopaque(page);
        BlockNumber next_block = opaque->btpo_next;

        if (next_block == 0) {
            // Need a new overflow page. Link it from the current page.
            MarkBufferDirty(buf);
            Buffer ovf_buf = hash_add_overflow_page(index, bucket);
            Buffer curr_buf = buf;
            buf = ovf_buf;
            page = BufferGetPage(buf);

            // Link the old page to the new overflow.
            Page curr_page = BufferGetPage(curr_buf);
            BTPageOpaque curr_opaque = _bt_page_getopaque(curr_page);
            curr_opaque->btpo_next = BufferGetBlockNumber(ovf_buf);
            MarkBufferDirty(curr_buf);
            ReleaseBuffer(curr_buf);
        } else {
            ReleaseBuffer(buf);
            buf =
                ReadBuffer(index->rd_smgr, ForkNumber::kMain, next_block, ReadBufferMode::kNormal);
            page = BufferGetPage(buf);
        }

        result = PageAddItem(page, reinterpret_cast<Item>(item_buf), item_size,
                             kInvalidOffsetNumber, false);
    }

    MarkBufferDirty(buf);
    ReleaseBuffer(buf);
    return true;
}

// --- Scan ---

BTScanDesc hashbeginscan(Relation index, BTKeyKind kind, const BTScanKeyData* scan_key) {
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

    auto* opaque = makePallocNode<HashScanOpaque>();
    scan->opaque = opaque;
    return scan;
}

bool hashgettuple(BTScanDesc scan) {
    Relation index = scan->index;
    index->rd_smgr = RelationGetSmgr(index);
    auto* so = static_cast<HashScanOpaque*>(scan->opaque);
    if (so == nullptr)
        return false;

    // On first call, position the scan at the starting bucket.
    if (!so->inited) {
        so->inited = true;

        if (scan->scan_key.strategy == BTStrategy::kEqual && scan->scan_key.key != nullptr) {
            // Equality lookup: go directly to the target bucket.
            Buffer meta_buf =
                ReadBuffer(index->rd_smgr, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
            Page meta_page = BufferGetPage(meta_buf);
            HashMetaPageData meta = hash_get_meta(meta_page);
            ReleaseBuffer(meta_buf);

            so->bucket =
                hashcalc(scan->key_kind, scan->scan_key.key, scan->scan_key.key_len, meta.nbuckets);
            BlockNumber block = hash_bucket_to_block(so->bucket);
            so->currbuf =
                ReadBuffer(index->rd_smgr, ForkNumber::kMain, block, ReadBufferMode::kNormal);
            so->curroff = 1;
        } else {
            // Full scan: start at bucket 0.
            so->bucket = 0;
            BlockNumber block = hash_bucket_to_block(0);
            so->currbuf =
                ReadBuffer(index->rd_smgr, ForkNumber::kMain, block, ReadBufferMode::kNormal);
            so->curroff = 1;
        }
    }

    // Walk the bucket's overflow chain, scanning each page for matches.
    while (so->currbuf != kInvalidBuffer) {
        Page page = BufferGetPage(so->currbuf);
        OffsetNumber max_offset = PageGetMaxOffsetNumber(page);

        while (so->curroff <= max_offset) {
            auto* item_id = PageGetItemId(page, so->curroff);
            so->curroff++;

            if (!ItemIdIsNormal(item_id))
                continue;

            auto* item = reinterpret_cast<BTItem>(PageGetItem(page, item_id));
            const void* entry_key = _bt_item_get_key(item);
            uint16_t entry_key_len = _bt_item_get_key_len(ItemIdGetLength(item_id));

            // For equality scan, check the key matches.
            if (scan->scan_key.strategy == BTStrategy::kEqual && scan->scan_key.key != nullptr) {
                int cmp = _bt_compare_keys(scan->key_kind, entry_key, entry_key_len,
                                           scan->scan_key.key, scan->scan_key.key_len);
                if (cmp != 0)
                    continue;
            }

            scan->curr_tid = item->tid;
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
        } else if (scan->scan_key.strategy != BTStrategy::kEqual) {
            // Full scan: advance to the next bucket.
            so->bucket++;
            Buffer meta_buf =
                ReadBuffer(index->rd_smgr, ForkNumber::kMain, 0, ReadBufferMode::kNormal);
            Page meta_page = BufferGetPage(meta_buf);
            HashMetaPageData meta = hash_get_meta(meta_page);
            ReleaseBuffer(meta_buf);

            if (so->bucket < meta.nbuckets) {
                BlockNumber block = hash_bucket_to_block(so->bucket);
                so->currbuf =
                    ReadBuffer(index->rd_smgr, ForkNumber::kMain, block, ReadBufferMode::kNormal);
                so->curroff = 1;
            }
        }
    }

    return false;
}

void hashrescan(BTScanDesc scan, const BTScanKeyData* new_scan_key) {
    auto* so = static_cast<HashScanOpaque*>(scan->opaque);
    if (so != nullptr) {
        if (so->currbuf != kInvalidBuffer) {
            ReleaseBuffer(so->currbuf);
            so->currbuf = kInvalidBuffer;
        }
        so->inited = false;
        so->curroff = 0;
    }
    if (new_scan_key != nullptr) {
        scan->scan_key = *new_scan_key;
    }
}

void hashendscan(BTScanDesc scan) {
    if (scan == nullptr)
        return;

    auto* so = static_cast<HashScanOpaque*>(scan->opaque);
    if (so != nullptr) {
        if (so->currbuf != kInvalidBuffer) {
            ReleaseBuffer(so->currbuf);
            so->currbuf = kInvalidBuffer;
        }
        destroyPallocNode(so);
    }
    destroyPallocNode(scan);
}

bool hashcanreturn([[maybe_unused]] Relation index) {
    // Hash indexes cannot return heap tuples (no amcanreturn in PostgreSQL).
    return false;
}

int64_t hashgetbitmap(BTScanDesc scan, std::vector<ItemPointerData>* tids) {
    if (scan == nullptr || tids == nullptr)
        return 0;
    int64_t count = 0;
    while (hashgettuple(scan)) {
        tids->push_back(scan->curr_tid);
        count++;
    }
    return count;
}

}  // namespace pgcpp::access

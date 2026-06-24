// heapam.cpp — Heap access method implementation.
//
// Converted from PostgreSQL 15's src/backend/access/heap/heapam.c.
//
// Implements heap_insert, heap_delete, heap_update, heap_beginscan,
// heap_getnext, and tuple formation/deformation.
//
// Key design decisions (preserved from PostgreSQL):
//   - Tuples are stored in heap pages (8KB) via PageAddItem
//   - MVCC metadata (t_xmin, t_xmax, t_cid) is set on insert/delete
//   - Scans check visibility via HeapTupleSatisfiesMVCC
//   - Visible tuples are cached per-page to avoid repeated visibility checks
//   - heap_insert extends the relation when no page has enough free space

#include "mytoydb/access/heapam.h"

#include <cstring>
#include <string>

#include "mytoydb/access/rel.h"
#include "mytoydb/catalog/pg_attribute.h"
#include "mytoydb/common/error/elog.h"
#include "mytoydb/common/memory/memory_context.h"
#include "mytoydb/storage/bufmgr.h"
#include "mytoydb/storage/bufpage.h"
#include "mytoydb/transaction/heap_tuple.h"
#include "mytoydb/transaction/snapshot.h"
#include "mytoydb/transaction/transam.h"
#include "mytoydb/transaction/visibility.h"
#include "mytoydb/transaction/xact.h"
#include "mytoydb/types/datum.h"

namespace mytoydb::access {

namespace {

using mytoydb::catalog::AttAlign;
using mytoydb::memory::palloc;
using mytoydb::memory::pfree;
using mytoydb::storage::BlockNumber;
using mytoydb::storage::Buffer;
using mytoydb::storage::BufferGetPage;
using mytoydb::storage::ForkNumber;
using mytoydb::storage::Item;
using mytoydb::storage::kBlckSz;
using mytoydb::storage::kInvalidBuffer;
using mytoydb::storage::kInvalidOffsetNumber;
using mytoydb::storage::kPageHeaderSize;
using mytoydb::storage::MarkBufferDirty;
using mytoydb::storage::OffsetNumber;
using mytoydb::storage::Page;
using mytoydb::storage::PageAddItem;
using mytoydb::storage::PageGetHeapFreeSpace;
using mytoydb::storage::PageGetItem;
using mytoydb::storage::PageGetItemId;
using mytoydb::storage::PageGetMaxOffsetNumber;
using mytoydb::storage::PageInit;
using mytoydb::storage::ReadBuffer;
using mytoydb::storage::ReadBufferMode;
using mytoydb::storage::ReleaseBuffer;
using mytoydb::transaction::CommandCounterIncrement;
using mytoydb::transaction::CommandId;
using mytoydb::transaction::GetCurrentCommandId;
using mytoydb::transaction::GetCurrentTransactionId;
using mytoydb::transaction::HeapTuple;
using mytoydb::transaction::HeapTupleData;
using mytoydb::transaction::HeapTupleHeaderData;
using mytoydb::transaction::HeapTupleHeaderSetCid;
using mytoydb::transaction::HeapTupleHeaderSetNatts;
using mytoydb::transaction::HeapTupleHeaderSetTid;
using mytoydb::transaction::HeapTupleHeaderSetXmax;
using mytoydb::transaction::HeapTupleHeaderSetXmin;
using mytoydb::transaction::HeapTupleSatisfiesMVCC;
using mytoydb::transaction::ItemPointerData;
using mytoydb::transaction::kHeapHasNull;
using mytoydb::transaction::kHeapHasVarWidth;
using mytoydb::transaction::kHeapTupleHeaderSize;
using mytoydb::transaction::kInvalidTransactionId;
using mytoydb::transaction::Snapshot;
using mytoydb::transaction::SnapshotData;
using mytoydb::transaction::TransactionId;
using mytoydb::types::Datum;
using mytoydb::types::DatumGetTextP;
using mytoydb::types::VARSIZE;

// Initialize a new page and extend the relation by one block.
Buffer CreateAndReadNewPage(Relation relation, BlockNumber block_num) {
    char pagebuf[kBlckSz];
    std::memset(pagebuf, 0, kBlckSz);
    PageInit(pagebuf, kBlckSz, 0);
    relation->rd_smgr = RelationGetSmgr(relation);
    mytoydb::storage::smgrextend(relation->rd_smgr, ForkNumber::kMain, block_num, pagebuf, false);
    return ReadBuffer(relation->rd_smgr, ForkNumber::kMain, block_num, ReadBufferMode::kNormal);
}

}  // namespace

// --- Alignment helpers ---

uint32_t att_align(uint32_t offset, AttAlign align) {
    switch (align) {
        case AttAlign::kChar:
            return offset;
        case AttAlign::kShort:
            return (offset + 1) & ~1u;
        case AttAlign::kInt:
            return (offset + 3) & ~3u;
        case AttAlign::kDouble:
            return (offset + 7) & ~7u;
    }
    return offset;
}

uint32_t att_align_max(uint32_t offset) {
    return (offset + 7) & ~7u;
}

// --- Tuple formation ---

uint32_t heap_compute_data_size(TupleDesc tupdesc, const Datum* values, const bool* isnull) {
    uint32_t data_size = 0;
    for (int i = 0; i < tupdesc->natts; i++) {
        if (isnull != nullptr && isnull[i])
            continue;
        const auto& attr = tupdesc->attrs[i];
        data_size = att_align(data_size, attr.attalign);
        if (attr.attlen == -1) {
            // varlena: 4-byte header + data
            data_size += static_cast<uint32_t>(VARSIZE(DatumGetTextP(values[i])));
        } else {
            data_size += static_cast<uint32_t>(attr.attlen);
        }
    }
    return data_size;
}

HeapTuple heap_form_tuple(TupleDesc tupdesc, const Datum* values, const bool* isnull) {
    // Check for nulls.
    bool hasnull = false;
    for (int i = 0; i < tupdesc->natts; i++) {
        if (isnull != nullptr && isnull[i]) {
            hasnull = true;
            break;
        }
    }

    uint32_t data_size = heap_compute_data_size(tupdesc, values, isnull);

    // Compute header size: fixed header + null bitmap (if any).
    uint32_t header_size = static_cast<uint32_t>(kHeapTupleHeaderSize);
    if (hasnull) {
        int null_bitmap_size = (tupdesc->natts + 7) / 8;
        header_size += static_cast<uint32_t>(null_bitmap_size);
    }
    uint32_t hoff = att_align_max(header_size);

    uint32_t tuple_size = hoff + data_size;

    // Allocate the tuple data (header + user data).
    char* data = static_cast<char*>(palloc(tuple_size));
    std::memset(data, 0, hoff);
    auto* header = reinterpret_cast<HeapTupleHeaderData*>(data);

    // Set header fields.
    header->t_hoff = static_cast<uint8_t>(hoff);
    HeapTupleHeaderSetNatts(header, tupdesc->natts);
    header->t_xmin = kInvalidTransactionId;
    header->t_xmax = kInvalidTransactionId;
    if (hasnull) {
        header->t_infomask |= kHeapHasNull;
    }

    // Fill null bitmap.
    if (hasnull) {
        uint8_t* null_bitmap = reinterpret_cast<uint8_t*>(data + kHeapTupleHeaderSize);
        for (int i = 0; i < tupdesc->natts; i++) {
            if (isnull != nullptr && isnull[i]) {
                null_bitmap[i / 8] |= static_cast<uint8_t>(1 << (i % 8));
            }
        }
    }

    // Copy data values.
    uint32_t offset = hoff;
    for (int i = 0; i < tupdesc->natts; i++) {
        if (isnull != nullptr && isnull[i])
            continue;
        const auto& attr = tupdesc->attrs[i];
        offset = att_align(offset, attr.attalign);

        if (attr.attlen == -1) {
            // varlena: copy the full varlena (header + data).
            const char* src = DatumGetTextP(values[i]);
            int len = VARSIZE(src);
            std::memcpy(data + offset, src, len);
            offset += len;
            header->t_infomask |= kHeapHasVarWidth;
        } else if (attr.attbyval) {
            // by-value: copy attlen bytes from the Datum.
            // On little-endian, the low bytes hold the value.
            std::memcpy(data + offset, &values[i], attr.attlen);
            offset += attr.attlen;
        } else {
            // by-reference, fixed length: copy from the pointer.
            std::memcpy(data + offset, reinterpret_cast<void*>(values[i]), attr.attlen);
            offset += attr.attlen;
            header->t_infomask |= kHeapHasVarWidth;
        }
    }

    // Allocate the HeapTupleData wrapper.
    void* tup_mem = palloc(sizeof(HeapTupleData));
    HeapTuple tuple = new (tup_mem) HeapTupleData();
    tuple->t_len = tuple_size;
    tuple->t_data = header;
    return tuple;
}

void heap_deform_tuple(HeapTuple tuple, TupleDesc tupdesc, Datum* values, bool* isnull) {
    HeapTupleHeaderData* header = tuple->t_data;
    char* data = reinterpret_cast<char*>(header);
    uint32_t hoff = header->t_hoff;

    bool hasnull = (header->t_infomask & kHeapHasNull) != 0;
    uint8_t* null_bitmap = nullptr;
    if (hasnull) {
        null_bitmap = reinterpret_cast<uint8_t*>(data + kHeapTupleHeaderSize);
    }

    uint32_t offset = hoff;
    for (int i = 0; i < tupdesc->natts; i++) {
        if (isnull != nullptr)
            isnull[i] = false;

        if (hasnull && (null_bitmap[i / 8] & (1 << (i % 8)))) {
            values[i] = 0;
            if (isnull != nullptr)
                isnull[i] = true;
            continue;
        }

        const auto& attr = tupdesc->attrs[i];
        offset = att_align(offset, attr.attalign);

        if (attr.attlen == -1) {
            // varlena: Datum points into the tuple.
            values[i] = Datum(data + offset);
            offset += static_cast<uint32_t>(VARSIZE(data + offset));
        } else if (attr.attbyval) {
            // by-value: copy attlen bytes into the Datum.
            Datum d = 0;
            std::memcpy(&d, data + offset, attr.attlen);
            values[i] = d;
            offset += attr.attlen;
        } else {
            // by-reference, fixed length: Datum points into the tuple.
            values[i] = Datum(data + offset);
            offset += attr.attlen;
        }
    }
}

Datum heap_getattr(HeapTuple tuple, int attnum, TupleDesc tupdesc, bool* isnull) {
    if (isnull != nullptr)
        *isnull = false;

    HeapTupleHeaderData* header = tuple->t_data;
    char* data = reinterpret_cast<char*>(header);
    uint32_t hoff = header->t_hoff;

    bool hasnull = (header->t_infomask & kHeapHasNull) != 0;
    uint8_t* null_bitmap = nullptr;
    if (hasnull) {
        null_bitmap = reinterpret_cast<uint8_t*>(data + kHeapTupleHeaderSize);
    }

    // Walk to the requested attribute (attributes are 1-based).
    uint32_t offset = hoff;
    for (int i = 0; i < attnum; i++) {
        if (i == attnum - 1) {
            // This is the attribute we want.
            if (hasnull && (null_bitmap[i / 8] & (1 << (i % 8)))) {
                if (isnull != nullptr)
                    *isnull = true;
                return 0;
            }
            const auto& attr = tupdesc->attrs[i];
            offset = att_align(offset, attr.attalign);

            if (attr.attlen == -1) {
                return Datum(data + offset);
            } else if (attr.attbyval) {
                Datum d = 0;
                std::memcpy(&d, data + offset, attr.attlen);
                return d;
            } else {
                return Datum(data + offset);
            }
        }

        // Skip this attribute.
        if (hasnull && (null_bitmap[i / 8] & (1 << (i % 8)))) {
            continue;
        }
        const auto& attr = tupdesc->attrs[i];
        offset = att_align(offset, attr.attalign);
        if (attr.attlen == -1) {
            offset += static_cast<uint32_t>(VARSIZE(data + offset));
        } else {
            offset += attr.attlen;
        }
    }

    if (isnull != nullptr)
        *isnull = true;
    return 0;
}

void heap_freetuple(HeapTuple tuple) {
    if (tuple == nullptr)
        return;
    if (tuple->t_data != nullptr) {
        pfree(tuple->t_data);
    }
    tuple->~HeapTupleData();
    pfree(tuple);
}

// --- Heap modification ---

// Internal insert logic without CommandCounterIncrement.
// Used by heap_insert and heap_update.
static ItemPointerData heap_insert_internal(Relation relation, HeapTuple tup) {
    TransactionId xid = GetCurrentTransactionId();
    CommandId cid = GetCurrentCommandId();

    HeapTupleHeaderData* header = tup->t_data;
    HeapTupleHeaderSetXmin(header, xid);
    HeapTupleHeaderSetCid(header, cid);
    HeapTupleHeaderSetXmax(header, kInvalidTransactionId);

    uint32_t tuple_len = tup->t_len;
    relation->rd_smgr = RelationGetSmgr(relation);

    // Find a page with enough free space.
    BlockNumber nblocks = RelationGetNumberOfBlocks(relation);
    Buffer buffer;
    Page page;
    BlockNumber target_block;

    if (nblocks == 0) {
        // Create the first page.
        target_block = 0;
        buffer = CreateAndReadNewPage(relation, target_block);
    } else {
        // Try the last block first.
        target_block = nblocks - 1;
        buffer =
            ReadBuffer(relation->rd_smgr, ForkNumber::kMain, target_block, ReadBufferMode::kNormal);
        page = BufferGetPage(buffer);
        if (static_cast<int>(PageGetHeapFreeSpace(page)) < static_cast<int>(tuple_len)) {
            // Not enough space; extend the relation.
            ReleaseBuffer(buffer);
            target_block = nblocks;
            buffer = CreateAndReadNewPage(relation, target_block);
        }
    }

    page = BufferGetPage(buffer);
    OffsetNumber offset =
        PageAddItem(page, reinterpret_cast<Item>(header), tuple_len, kInvalidOffsetNumber, true);
    if (offset == kInvalidOffsetNumber) {
        ReleaseBuffer(buffer);
        ereport(mytoydb::error::LogLevel::kError,
                "heap_insert: failed to add item to page " + std::to_string(target_block));
    }

    // Set the TID.
    ItemPointerData tid;
    tid.ip_blkid = target_block;
    tid.ip_posid = offset;
    tup->t_self = tid;
    HeapTupleHeaderSetTid(header, tid);

    MarkBufferDirty(buffer);
    ReleaseBuffer(buffer);

    return tid;
}

ItemPointerData heap_insert(Relation relation, HeapTuple tup) {
    ItemPointerData tid = heap_insert_internal(relation, tup);
    CommandCounterIncrement();
    return tid;
}

void heap_delete(Relation relation, const ItemPointerData& tid) {
    BlockNumber block_num = tid.ip_blkid;
    OffsetNumber offset = tid.ip_posid;

    relation->rd_smgr = RelationGetSmgr(relation);
    Buffer buffer =
        ReadBuffer(relation->rd_smgr, ForkNumber::kMain, block_num, ReadBufferMode::kNormal);
    Page page = BufferGetPage(buffer);

    auto* item_id = PageGetItemId(page, offset);
    HeapTupleHeaderData* header =
        reinterpret_cast<HeapTupleHeaderData*>(PageGetItem(page, item_id));

    TransactionId xid = GetCurrentTransactionId();
    CommandId cid = GetCurrentCommandId();
    HeapTupleHeaderSetXmax(header, xid);
    HeapTupleHeaderSetCid(header, cid);

    MarkBufferDirty(buffer);
    ReleaseBuffer(buffer);

    CommandCounterIncrement();
}

ItemPointerData heap_update(Relation relation, const ItemPointerData& otid, HeapTuple tup) {
    // Insert the new tuple first (without CID increment).
    ItemPointerData new_tid = heap_insert_internal(relation, tup);

    // Mark the old tuple as deleted and point its t_ctid to the new tuple.
    BlockNumber block_num = otid.ip_blkid;
    OffsetNumber offset = otid.ip_posid;

    relation->rd_smgr = RelationGetSmgr(relation);
    Buffer buffer =
        ReadBuffer(relation->rd_smgr, ForkNumber::kMain, block_num, ReadBufferMode::kNormal);
    Page page = BufferGetPage(buffer);

    auto* item_id = PageGetItemId(page, offset);
    HeapTupleHeaderData* header =
        reinterpret_cast<HeapTupleHeaderData*>(PageGetItem(page, item_id));

    TransactionId xid = GetCurrentTransactionId();
    CommandId cid = GetCurrentCommandId();
    HeapTupleHeaderSetXmax(header, xid);
    HeapTupleHeaderSetCid(header, cid);
    HeapTupleHeaderSetTid(header, new_tid);

    MarkBufferDirty(buffer);
    ReleaseBuffer(buffer);

    CommandCounterIncrement();
    return new_tid;
}

// --- Heap scan ---

HeapScanDesc heap_beginscan(Relation relation, Snapshot snapshot) {
    if (snapshot == nullptr) {
        snapshot = mytoydb::transaction::GetTransactionSnapshot();
    }
    void* mem = palloc(sizeof(HeapScanDescData));
    HeapScanDesc scan = new (mem) HeapScanDescData();

    scan->rs_base = relation;
    scan->rs_snapshot = snapshot;
    scan->rs_nblocks = RelationGetNumberOfBlocks(relation);
    scan->rs_cblock = 0;
    scan->rs_cbuf = kInvalidBuffer;
    scan->rs_coffset = 0;
    scan->rs_inited = false;
    scan->rs_ntuples = 0;
    scan->rs_vistuple_index = 0;

    return scan;
}

// Scan the current page and cache visible tuple offsets.
static void heap_scan_page(HeapScanDesc scan, BlockNumber block_num) {
    Relation relation = scan->rs_base;
    relation->rd_smgr = RelationGetSmgr(relation);

    // Release the previous buffer if any.
    if (scan->rs_cbuf != kInvalidBuffer) {
        ReleaseBuffer(scan->rs_cbuf);
        scan->rs_cbuf = kInvalidBuffer;
    }

    scan->rs_cblock = block_num;
    scan->rs_cbuf =
        ReadBuffer(relation->rd_smgr, ForkNumber::kMain, block_num, ReadBufferMode::kNormal);
    Page page = BufferGetPage(scan->rs_cbuf);

    scan->rs_ntuples = 0;
    scan->rs_vistuple_index = 0;

    OffsetNumber max_offset = PageGetMaxOffsetNumber(page);
    const SnapshotData& snap = *scan->rs_snapshot;

    for (OffsetNumber offset = 1; offset <= max_offset; offset++) {
        auto* item_id = PageGetItemId(page, offset);
        if (!mytoydb::storage::ItemIdIsNormal(item_id))
            continue;

        HeapTupleHeaderData* header =
            reinterpret_cast<HeapTupleHeaderData*>(PageGetItem(page, item_id));

        if (HeapTupleSatisfiesMVCC(header, snap)) {
            if (scan->rs_ntuples < kHeapTuplesPerPage) {
                scan->rs_vistuples[scan->rs_ntuples] = offset;
                scan->rs_ntuples++;
            }
        }
    }
}

HeapTuple heap_getnext(HeapScanDesc scan) {
    // If we have cached visible tuples, return the next one.
    while (scan->rs_vistuple_index >= scan->rs_ntuples) {
        // Need to advance to the next page.
        BlockNumber next_block;

        if (!scan->rs_inited) {
            // First call: start at block 0.
            next_block = 0;
            scan->rs_inited = true;
        } else {
            // Move to the next block.
            next_block = scan->rs_cblock + 1;
        }

        if (next_block >= scan->rs_nblocks) {
            // Scan complete.
            return nullptr;
        }

        heap_scan_page(scan, next_block);
    }

    // Return the next visible tuple from the cache.
    OffsetNumber offset = scan->rs_vistuples[scan->rs_vistuple_index];
    scan->rs_vistuple_index++;

    Page page = BufferGetPage(scan->rs_cbuf);
    auto* item_id = PageGetItemId(page, offset);
    HeapTupleHeaderData* header =
        reinterpret_cast<HeapTupleHeaderData*>(PageGetItem(page, item_id));

    // Fill in the scan's tuple descriptor.
    scan->rs_ctup.t_len = mytoydb::storage::ItemIdGetLength(item_id);
    scan->rs_ctup.t_self.ip_blkid = scan->rs_cblock;
    scan->rs_ctup.t_self.ip_posid = offset;
    scan->rs_ctup.t_data = header;

    return &scan->rs_ctup;
}

void heap_endscan(HeapScanDesc scan) {
    if (scan == nullptr)
        return;

    if (scan->rs_cbuf != kInvalidBuffer) {
        ReleaseBuffer(scan->rs_cbuf);
        scan->rs_cbuf = kInvalidBuffer;
    }

    scan->~HeapScanDescData();
    pfree(scan);
}

void heap_rescan(HeapScanDesc scan) {
    if (scan->rs_cbuf != kInvalidBuffer) {
        ReleaseBuffer(scan->rs_cbuf);
        scan->rs_cbuf = kInvalidBuffer;
    }

    scan->rs_nblocks = RelationGetNumberOfBlocks(scan->rs_base);
    scan->rs_cblock = 0;
    scan->rs_coffset = 0;
    scan->rs_inited = false;
    scan->rs_ntuples = 0;
    scan->rs_vistuple_index = 0;
}

}  // namespace mytoydb::access

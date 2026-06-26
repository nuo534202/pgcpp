// heapam.h — Heap access method API.
//
// Converted from PostgreSQL 15's src/include/access/heapam.h.
//
// The heap access method provides the operations for ordinary tables:
//   heap_insert  — insert a tuple into a heap
//   heap_delete  — mark a tuple as deleted (set t_xmax)
//   heap_update  — replace a tuple with a new version
//   heap_beginscan — start a sequential scan
//   heap_getnext  — fetch the next visible tuple from a scan
//   heap_endscan  — release scan resources
//
// Tuple formation:
//   heap_form_tuple   — build a HeapTuple from Datums + null array
//   heap_deform_tuple — extract Datums from a HeapTuple
//
// All operations respect MVCC visibility: inserts record the current XID
// as t_xmin, scans check visibility against the snapshot.
#pragma once

#include <cstdint>

#include "mytoydb/access/rel.hpp"
#include "mytoydb/storage/bufmgr.hpp"
#include "mytoydb/storage/bufpage.hpp"
#include "mytoydb/transaction/heap_tuple.hpp"
#include "mytoydb/transaction/snapshot.hpp"
#include "mytoydb/transaction/transam.hpp"
#include "mytoydb/transaction/xact.hpp"
#include "mytoydb/types/datum.hpp"

namespace mytoydb::access {

// ScanDirection — direction for index and heap scans.
enum class ScanDirection {
    kForward,     // scan forward (ascending)
    kBackward,    // scan backward (descending)
    kNoMovement,  // no movement (rescan to current position)
};

// Maximum number of visible tuples cached per page during a scan.
// PostgreSQL uses HEAPTUPLES_PER_PAGE (typically 291 for 8KB pages).
// MyToyDB uses a smaller value for simplicity.
constexpr int kHeapTuplesPerPage = 256;

// HeapScanDescData — descriptor for a heap sequential scan.
//
// Fields mirror PostgreSQL's HeapScanDescData:
//   rs_base      — the relation being scanned
//   rs_snapshot  — MVCC snapshot for visibility checks
//   rs_nblocks   — total number of blocks in the relation
//   rs_cblock    — current block number being scanned
//   rs_cbuf      — current buffer (pinned while scanning the page)
//   rs_coffset   — next offset to examine on the current page
//   rs_inited    — true if the scan has been positioned (first page read)
//   rs_vistuples — cached array of visible tuple offsets on the current page
//   rs_ntuples   — number of valid entries in rs_vistuples
//   rs_vistuple_index — index of the next tuple to return from rs_vistuples
struct HeapScanDescData {
    Relation rs_base = nullptr;
    mytoydb::transaction::Snapshot rs_snapshot = nullptr;

    mytoydb::storage::BlockNumber rs_nblocks = 0;
    mytoydb::storage::BlockNumber rs_cblock = 0;
    mytoydb::storage::Buffer rs_cbuf = mytoydb::storage::kInvalidBuffer;
    mytoydb::storage::OffsetNumber rs_coffset = 0;
    bool rs_inited = false;

    // Cache of visible tuple offsets on the current page.
    mytoydb::storage::OffsetNumber rs_vistuples[kHeapTuplesPerPage] = {};
    int rs_ntuples = 0;
    int rs_vistuple_index = 0;

    // The current tuple being returned to the caller. Points into the
    // buffer page (valid until the next heap_getnext call).
    mytoydb::transaction::HeapTupleData rs_ctup;
};

// HeapScanDesc — pointer to a HeapScanDescData.
using HeapScanDesc = HeapScanDescData*;

// --- Heap modification operations ---

// heap_insert — insert a tuple into a heap relation.
//
// The tuple's t_data must be filled with user data (via heap_form_tuple).
// This function:
//   1. Sets t_xmin = current transaction ID, t_cid = current command ID
//   2. Finds a page with enough free space (or extends the relation)
//   3. Adds the tuple to the page via PageAddItem
//   4. Sets the tuple's t_self (TID) to the inserted location
//   5. Marks the buffer dirty and releases it
//
// The tuple's t_data is modified in place (header fields are set).
// Returns the TID of the inserted tuple.
mytoydb::transaction::ItemPointerData heap_insert(Relation relation,
                                                  mytoydb::transaction::HeapTuple tup);

// heap_delete — mark a tuple as deleted.
//
// Sets t_xmax = current transaction ID on the tuple identified by tid.
// The tuple is not physically removed (that's VACUUM's job); it becomes
// invisible to new snapshots.
void heap_delete(Relation relation, const mytoydb::transaction::ItemPointerData& tid);

// heap_update — replace a tuple with a new version.
//
// Marks the old tuple as deleted (sets t_xmax) and inserts the new tuple.
// The old tuple's t_ctid is updated to point to the new tuple.
// Returns the TID of the new tuple.
mytoydb::transaction::ItemPointerData heap_update(Relation relation,
                                                  const mytoydb::transaction::ItemPointerData& otid,
                                                  mytoydb::transaction::HeapTuple tup);

// --- Heap scan operations ---

// heap_beginscan — start a sequential scan on a heap relation.
//
// Allocates a HeapScanDesc, computes the number of blocks, and positions
// the scan before the first block. The snapshot determines visibility.
HeapScanDesc heap_beginscan(Relation relation, mytoydb::transaction::Snapshot snapshot);

// heap_getnext — fetch the next visible tuple from a scan.
//
// Scans pages in forward order, checking each tuple for visibility.
// Returns nullptr when the scan is complete.
// The returned HeapTuple points into the scan descriptor's tuple buffer;
// the caller must not free it.
mytoydb::transaction::HeapTuple heap_getnext(HeapScanDesc scan);

// heap_endscan — release scan resources (unpin the current buffer, free
// the descriptor).
void heap_endscan(HeapScanDesc scan);

// heap_rescan — restart a scan from the beginning.
void heap_rescan(HeapScanDesc scan);

// --- Tuple formation ---

// heap_form_tuple — build a HeapTuple from column values.
//
// Lays out the tuple data according to the tuple descriptor's alignment
// rules. The returned HeapTuple is palloc'd in the current memory context.
// The caller must set t_self separately (heap_insert does this).
mytoydb::transaction::HeapTuple heap_form_tuple(TupleDesc tupdesc,
                                                const mytoydb::types::Datum* values,
                                                const bool* isnull);

// heap_deform_tuple — extract column values from a HeapTuple.
//
// Fills the values[] and isnull[] arrays (which must have tupdesc->natts
// entries). For by-reference types, the Datum points into the tuple's data
// (valid as long as the tuple is pinned).
void heap_deform_tuple(mytoydb::transaction::HeapTuple tuple, TupleDesc tupdesc,
                       mytoydb::types::Datum* values, bool* isnull);

// heap_freetuple — free a HeapTuple allocated by heap_form_tuple.
void heap_freetuple(mytoydb::transaction::HeapTuple tuple);

// --- Helpers ---

// heap_getattr — extract a single attribute value from a tuple.
// Returns the Datum for the attribute (attnum is 1-based).
// Sets *isnull if the attribute is null.
mytoydb::types::Datum heap_getattr(mytoydb::transaction::HeapTuple tuple, int attnum,
                                   TupleDesc tupdesc, bool* isnull);

// Compute the data portion size of a tuple (excluding header).
// Used by heap_form_tuple to allocate the right amount of memory.
uint32_t heap_compute_data_size(TupleDesc tupdesc, const mytoydb::types::Datum* values,
                                const bool* isnull);

// Align an offset to the given alignment type.
uint32_t att_align(uint32_t offset, mytoydb::catalog::AttAlign align);

// Align an offset to MAXALIGN (8 bytes).
uint32_t att_align_max(uint32_t offset);

}  // namespace mytoydb::access

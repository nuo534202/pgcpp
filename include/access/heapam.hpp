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

#include "access/rel.hpp"
#include "storage/bufmgr.hpp"
#include "storage/bufpage.hpp"
#include "transaction/heap_tuple.hpp"
#include "transaction/snapshot.hpp"
#include "transaction/transam.hpp"
#include "transaction/xact.hpp"
#include "types/datum.hpp"

namespace pgcpp::access {

// --- System column attribute numbers (PostgreSQL-compatible, negative) ---
//
// These mirror PostgreSQL's SelfItemPointerAttributeNumber etc. from
// src/include/access/sysattr.h. They are passed as `attnum` to
// heap_tuple_buffer_getsysattr and heap_attisnull to request system columns.
constexpr int kSelfItemPointerAttributeNumber = -1;   // ctid
constexpr int kMinTransactionIdAttributeNumber = -2;  // xmin
constexpr int kMaxTransactionIdAttributeNumber = -3;  // xmax
constexpr int kMinCommandIdAttributeNumber = -4;      // cmin
constexpr int kMaxCommandIdAttributeNumber = -5;      // cmax
constexpr int kTableOidAttributeNumber = -6;          // tableoid

// ScanDirection — direction for index and heap scans.
enum class ScanDirection {
    kForward,     // scan forward (ascending)
    kBackward,    // scan backward (descending)
    kNoMovement,  // no movement (rescan to current position)
};

// Maximum number of visible tuples cached per page during a scan.
// PostgreSQL uses HEAPTUPLES_PER_PAGE (typically 291 for 8KB pages).
// pgcpp uses a smaller value for simplicity.
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
    pgcpp::transaction::Snapshot rs_snapshot = nullptr;

    pgcpp::storage::BlockNumber rs_nblocks = 0;
    pgcpp::storage::BlockNumber rs_cblock = 0;
    pgcpp::storage::Buffer rs_cbuf = pgcpp::storage::kInvalidBuffer;
    pgcpp::storage::OffsetNumber rs_coffset = 0;
    bool rs_inited = false;

    // Cache of visible tuple offsets on the current page.
    pgcpp::storage::OffsetNumber rs_vistuples[kHeapTuplesPerPage] = {};
    int rs_ntuples = 0;
    int rs_vistuple_index = 0;

    // Cache invalidation: when the command ID advances or the active snapshot
    // changes, the per-page visibility cache must be rebuilt. Set to
    // kInvalidCommandId to force a rebuild on the next heap_getnext.
    pgcpp::transaction::CommandId rs_cached_cnum = pgcpp::transaction::kInvalidCommandId;
    pgcpp::transaction::Snapshot rs_cached_snapshot = nullptr;

    // The current tuple being returned to the caller. Points into the
    // buffer page (valid until the next heap_getnext call).
    pgcpp::transaction::HeapTupleData rs_ctup;
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
pgcpp::transaction::ItemPointerData heap_insert(Relation relation,
                                                pgcpp::transaction::HeapTuple tup);

// heap_delete — mark a tuple as deleted.
//
// Sets t_xmax = current transaction ID on the tuple identified by tid.
// The tuple is not physically removed (that's VACUUM's job); it becomes
// invisible to new snapshots.
void heap_delete(Relation relation, const pgcpp::transaction::ItemPointerData& tid);

// heap_update — replace a tuple with a new version.
//
// Marks the old tuple as deleted (sets t_xmax) and inserts the new tuple.
// The old tuple's t_ctid is updated to point to the new tuple.
// Returns the TID of the new tuple.
pgcpp::transaction::ItemPointerData heap_update(Relation relation,
                                                const pgcpp::transaction::ItemPointerData& otid,
                                                pgcpp::transaction::HeapTuple tup);

// --- Heap scan operations ---

// heap_beginscan — start a sequential scan on a heap relation.
//
// Allocates a HeapScanDesc, computes the number of blocks, and positions
// the scan before the first block. The snapshot determines visibility.
HeapScanDesc heap_beginscan(Relation relation, pgcpp::transaction::Snapshot snapshot);

// heap_getnext — fetch the next visible tuple from a scan.
//
// Scans pages in forward order, checking each tuple for visibility.
// Returns nullptr when the scan is complete.
// The returned HeapTuple points into the scan descriptor's tuple buffer;
// the caller must not free it.
pgcpp::transaction::HeapTuple heap_getnext(HeapScanDesc scan);

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
pgcpp::transaction::HeapTuple heap_form_tuple(TupleDesc tupdesc, const pgcpp::types::Datum* values,
                                              const bool* isnull);

// heap_deform_tuple — extract column values from a HeapTuple.
//
// Fills the values[] and isnull[] arrays (which must have tupdesc->natts
// entries). For by-reference types, the Datum points into the tuple's data
// (valid as long as the tuple is pinned).
void heap_deform_tuple(pgcpp::transaction::HeapTuple tuple, TupleDesc tupdesc,
                       pgcpp::types::Datum* values, bool* isnull);

// heap_freetuple — free a HeapTuple allocated by heap_form_tuple.
void heap_freetuple(pgcpp::transaction::HeapTuple tuple);

// --- Helpers ---

// heap_getattr — extract a single attribute value from a tuple.
// Returns the Datum for the attribute (attnum is 1-based).
// Sets *isnull if the attribute is null.
pgcpp::types::Datum heap_getattr(pgcpp::transaction::HeapTuple tuple, int attnum, TupleDesc tupdesc,
                                 bool* isnull);

// Compute the data portion size of a tuple (excluding header).
// Used by heap_form_tuple to allocate the right amount of memory.
uint32_t heap_compute_data_size(TupleDesc tupdesc, const pgcpp::types::Datum* values,
                                const bool* isnull);

// Align an offset to the given alignment type.
uint32_t att_align(uint32_t offset, pgcpp::catalog::AttAlign align);

// Align an offset to MAXALIGN (8 bytes).
uint32_t att_align_max(uint32_t offset);

// --- heaptuple.c P0 extensions (Task 15.8.1 / GAP-M8-F02) ---
//
// These mirror PostgreSQL's src/backend/access/common/heaptuple.c helpers,
// preserved as C++20 free functions. They operate on the same HeapTupleHeader
// layout described in heap_tuple.h.

// heap_fill_tuple — fill a pre-allocated tuple data buffer with column values.
//
// `data` must point to a buffer of at least `hoff + data_size` bytes, where
// `hoff` is the aligned header size (computed by the caller from the null
// bitmap; this function writes the null bitmap at `data + kHeapTupleHeaderSize`
// and the column data at `data + hoff`). The header bytes [0, hoff) are
// zeroed by this function so that the null bitmap region is clean.
//
// Outputs:
//   *infomask_out    — receives the HEAP_HASNULL / HEAP_HASVARWIDTH bits
//   *tuple_hoff_out  — receives the computed hoff (aligned header size)
void heap_fill_tuple(TupleDesc tupdesc, const pgcpp::types::Datum* values, const bool* isnull,
                     char* data, uint32_t data_size, uint16_t* infomask_out,
                     uint8_t* tuple_hoff_out);

// heap_modify_tuple — build a new HeapTuple by selectively replacing columns.
//
// For each column i: if `do_replace[i]` is true, the new value/isnull is used;
// otherwise the original value from `tuple` is preserved. If `do_replace` is
// nullptr, every column is replaced (equivalent to heap_form_tuple).
pgcpp::transaction::HeapTuple heap_modify_tuple(pgcpp::transaction::HeapTuple tuple,
                                                TupleDesc tupdesc,
                                                const pgcpp::types::Datum* values,
                                                const bool* isnull, const bool* do_replace);

// heap_modify_tuple_by_cols — replace only the first `ncols` columns.
// Columns beyond `ncols` keep their original values.
pgcpp::transaction::HeapTuple heap_modify_tuple_by_cols(pgcpp::transaction::HeapTuple tuple,
                                                        TupleDesc tupdesc, int ncols,
                                                        const pgcpp::types::Datum* values,
                                                        const bool* isnull);

// heap_copytuple — deep-copy a HeapTuple (wrapper + t_data buffer).
pgcpp::transaction::HeapTuple heap_copytuple(pgcpp::transaction::HeapTuple tuple);

// heap_copytuple_with_tuple — copy src into dest's wrapper, allocating a fresh
// t_data buffer. dest must already be allocated (e.g. via makePallocNode).
void heap_copytuple_with_tuple(pgcpp::transaction::HeapTuple src,
                               pgcpp::transaction::HeapTuple dest);

// heap_attisnull — true if attribute `attnum` (1-based user column) is NULL.
// Negative attnum values request system columns (always non-NULL in pgcpp).
bool heap_attisnull(pgcpp::transaction::HeapTuple tuple, int attnum, TupleDesc tupdesc);

// Minimal tuple conversions.
//
// pgcpp simplification: a minimal tuple uses the same on-disk layout as a
// heap tuple (HeapTupleHeaderData + data). The conversion functions are
// therefore deep copies. PostgreSQL's real minimal-tuple format omits the
// t_ctid field to save space, but that optimization is unnecessary for
// ClickBench and the _tuplesort/tuplestore callers accept this simplification.
pgcpp::transaction::HeapTuple minimal_tuple_from_heap_tuple(pgcpp::transaction::HeapTuple tuple);
pgcpp::transaction::HeapTuple heap_tuple_from_minimal_tuple(pgcpp::transaction::HeapTuple mtup);

// heap_tuple_buffer_getsysattr — extract a system column Datum.
//
// Supports the five core system columns (tableoid, xmin, xmax, cmin, cmax,
// ctid). `attnum` is negative (see kSelfItemPointerAttributeNumber etc.).
// Sets *isnull to false for all supported system columns. ereport(ERROR) for
// unsupported system columns.
pgcpp::types::Datum heap_tuple_buffer_getsysattr(pgcpp::transaction::HeapTuple tuple, int attnum,
                                                 TupleDesc tupdesc, bool* isnull);

// InvalidateAllVisibilityCaches — mark all active scans' visibility caches
// as stale so the next heap_getnext rebuilds them. Called by
// CommandCounterIncrement so that subsequent scans see tuples
// inserted/deleted by the previous command in the same transaction.
// Per-backend (scans are per-process; no shared memory involved).
void InvalidateAllVisibilityCaches();

}  // namespace pgcpp::access

// indextuple.h — Index tuple (B-tree entry) representation.
//
// Converted from PostgreSQL 15's src/include/access/itup.h.
//
// An index tuple is a single entry in a B-tree (or other) index. It carries:
//   1. IndexTupleData header (8 bytes) — TID pointing to the heap tuple +
//      a t_info word encoding the tuple size and flags (has-nulls, has-varlen)
//   2. Optional null bitmap (only if INDEX_NULL_MASK is set)
//   3. Index key data (column values, aligned per type)
//
// MyToyDB preserves PostgreSQL's on-disk header layout so the storage format
// is compatible. The key-data layout follows the same alignment rules as heap
// tuples (see heapam.cpp's att_align), starting after the (8-byte) header and
// optional null bitmap.
#pragma once

#include <cstdint>

#include "pgcpp/access/rel.hpp"
#include "pgcpp/storage/bufpage.hpp"
#include "pgcpp/transaction/heap_tuple.hpp"
#include "pgcpp/types/datum.hpp"

namespace mytoydb::access {

// --- t_info flag bits (PostgreSQL-compatible) ---
//
// The low 14 bits store the tuple size (including the header); the high two
// bits encode the has-nulls / has-varlen flags.
constexpr uint16_t kIndexSizeMask = 0x3FFF;    // size mask (14 bits)
constexpr uint16_t kIndexVarMask = 0x4000;     // has variable-width attributes
constexpr uint16_t kIndexNullMask = 0x8000;    // has null bitmap
constexpr uint16_t kIndexSizeMaxAttr = 0x3FF;  // limit on number of attrs

// IndexTupleData — the on-disk header of an index tuple.
//
// Layout (matches PostgreSQL for on-disk compatibility):
//   Offset  Size  Field
//   0       6     t_tid (ItemPointerData: 4-byte block + 2-byte offset)
//   6       2     t_info (size + flags)
//
// Total header size: 8 bytes (already 8-byte aligned, so no padding needed).
struct IndexTupleData {
    mytoydb::transaction::ItemPointerData t_tid;  // 6 bytes — TID of heap tuple
    uint16_t t_info = 0;                          // size + flags
    // null bitmap follows if kIndexNullMask is set (variable length)
    // key data follows the null bitmap
};

// IndexTuple — pointer to an IndexTupleData (PostgreSQL convention).
using IndexTuple = IndexTupleData*;

// Size of the fixed IndexTupleData header (excluding the null bitmap).
// PostgreSQL uses sizeof(IndexTupleData) which is 8 due to packing.
constexpr int kIndexTupleHeaderSize = 8;
// The header is already 8-byte aligned (MAXALIGN).
constexpr int kIndexTupleHeaderAligned = 8;

// --- Inline accessors (mirror PostgreSQL's IndexTupleSize etc.) ---

// IndexTupleSize — total size of the index tuple (header + bitmap + data).
inline uint16_t IndexTupleSize(const IndexTupleData* tup) {
    return tup->t_info & kIndexSizeMask;
}

// IndexTupleHasNulls — true if the tuple has a null bitmap.
inline bool IndexTupleHasNulls(const IndexTupleData* tup) {
    return (tup->t_info & kIndexNullMask) != 0;
}

// IndexTupleHasVarwidth — true if the tuple has variable-width attributes.
inline bool IndexTupleHasVarwidth(const IndexTupleData* tup) {
    return (tup->t_info & kIndexVarMask) != 0;
}

// --- Construction / deformation ---

// index_form_tuple — build an IndexTuple from column values.
//
// Lays out the index key data according to the tuple descriptor's alignment
// rules. The returned IndexTuple is palloc'd in the current memory context.
// The TID is set from the `tid` parameter (the caller already knows which
// heap tuple this index entry points to).
IndexTuple index_form_tuple(TupleDesc tupdesc, const mytoydb::types::Datum* values,
                            const bool* isnull, const mytoydb::transaction::ItemPointerData& tid);

// index_deform_tuple — extract column values from an IndexTuple.
//
// Fills the values[] and isnull[] arrays (which must have tupdesc->natts
// entries). For by-reference types, the Datum points into the tuple's data
// (valid as long as the tuple is pinned).
void index_deform_tuple(IndexTuple tup, TupleDesc tupdesc, mytoydb::types::Datum* values,
                        bool* isnull);

// --- Helpers ---

// Compute the data portion size of an index tuple (excluding header + bitmap).
uint32_t index_compute_data_size(TupleDesc tupdesc, const mytoydb::types::Datum* values,
                                 const bool* isnull);

// Copy an index tuple (deep copy of header + bitmap + data).
IndexTuple CopyIndexTuple(IndexTuple source);

}  // namespace mytoydb::access

// tupletable.h — TupleTableSlot: the executor's universal tuple container.
//
// Converted from PostgreSQL 15's src/include/executor/tuptable.h.
//
// A TupleTableSlot holds a tuple in deformed (Datum array) form. It can
// wrap either:
//   - a physical HeapTuple (from a heap scan or DML), or
//   - a virtual tuple (Datum values computed by a projection).
//
// The slot owns the deformed values/isnull arrays (palloc'd) and
// optionally owns the backing HeapTuple (when tts_shouldFree is true).
#pragma once

#include <cstdint>

#include "mytoydb/access/rel.h"
#include "mytoydb/common/memory/memory_context.h"
#include "mytoydb/transaction/heap_tuple.h"
#include "mytoydb/types/datum.h"

namespace mytoydb::executor {

// TupleTableSlot — holds a tuple in deformed form.
struct TupleTableSlot {
    mytoydb::access::TupleDesc tts_tupleDescriptor = nullptr;
    mytoydb::transaction::HeapTuple tts_tuple = nullptr;  // backing tuple
    mytoydb::types::Datum* tts_values = nullptr;          // deformed values
    bool* tts_isnull = nullptr;                           // null flags
    bool tts_shouldFree = false;                          // free tts_tuple on Clear?
    bool tts_shouldFreeMin = false;                       // free minimal tuple? (unused)
    bool tts_slow = false;                                // slow deform path used? (unused)
    bool tts_nvalid = false;                              // are tts_values/tts_isnull valid?
    bool tts_isempty = true;                              // slot is empty (no tuple stored)

    // Create a slot for the given tuple descriptor.
    // Allocates tts_values and tts_isnull arrays via palloc.
    static TupleTableSlot* Make(mytoydb::access::TupleDesc tupdesc);

    // Store a physical heap tuple into the slot, deforming it.
    void StoreTuple(mytoydb::transaction::HeapTuple tuple, bool shouldFree);

    // Store virtual values (no backing tuple).
    void StoreVirtual(const mytoydb::types::Datum* values, const bool* isnull);

    // Clear the slot (free backing tuple if shouldFree).
    void Clear();

    // Number of attributes in the slot.
    int Natts() const { return tts_tupleDescriptor ? tts_tupleDescriptor->natts : 0; }

    ~TupleTableSlot();
};

// --- PostgreSQL-compatible API names ---

inline TupleTableSlot* MakeTupleTableSlot(mytoydb::access::TupleDesc tupdesc) {
    return TupleTableSlot::Make(tupdesc);
}

inline void ExecStoreTuple(mytoydb::transaction::HeapTuple tuple, TupleTableSlot* slot,
                           bool shouldFree) {
    slot->StoreTuple(tuple, shouldFree);
}

inline void ExecClearTuple(TupleTableSlot* slot) {
    slot->Clear();
}

}  // namespace mytoydb::executor

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

#include "access/rel.hpp"
#include "common/memory/memory_context.hpp"
#include "transaction/heap_tuple.hpp"
#include "types/datum.hpp"

namespace pgcpp::executor {

// TupleTableSlot — holds a tuple in deformed form.
struct TupleTableSlot {
    pgcpp::access::TupleDesc tts_tupleDescriptor = nullptr;
    pgcpp::transaction::HeapTuple tts_tuple = nullptr;  // backing tuple
    pgcpp::types::Datum* tts_values = nullptr;          // deformed values
    bool* tts_isnull = nullptr;                         // null flags
    bool tts_shouldFree = false;                        // free tts_tuple on Clear?
    bool tts_shouldFreeMin = false;                     // free minimal tuple? (unused)
    bool tts_slow = false;                              // slow deform path used? (unused)
    bool tts_nvalid = false;                            // are tts_values/tts_isnull valid?
    bool tts_isempty = true;                            // slot is empty (no tuple stored)

    // Create a slot for the given tuple descriptor.
    // Allocates tts_values and tts_isnull arrays via palloc.
    static TupleTableSlot* Make(pgcpp::access::TupleDesc tupdesc);

    // Store a physical heap tuple into the slot, deforming it.
    void StoreTuple(pgcpp::transaction::HeapTuple tuple, bool shouldFree);

    // Store virtual values (no backing tuple).
    void StoreVirtual(const pgcpp::types::Datum* values, const bool* isnull);

    // Clear the slot (free backing tuple if shouldFree).
    void Clear();

    // Number of attributes in the slot.
    int Natts() const { return tts_tupleDescriptor ? tts_tupleDescriptor->natts : 0; }

    ~TupleTableSlot();
};

// --- PostgreSQL-compatible API names ---

inline TupleTableSlot* MakeTupleTableSlot(pgcpp::access::TupleDesc tupdesc) {
    return TupleTableSlot::Make(tupdesc);
}

inline void ExecStoreTuple(pgcpp::transaction::HeapTuple tuple, TupleTableSlot* slot,
                           bool shouldFree) {
    slot->StoreTuple(tuple, shouldFree);
}

inline void ExecClearTuple(TupleTableSlot* slot) {
    slot->Clear();
}

}  // namespace pgcpp::executor

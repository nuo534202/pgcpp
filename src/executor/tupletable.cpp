// tupletable.cpp — TupleTableSlot implementation.
//
// Converted from PostgreSQL 15's src/backend/executor/execTuples.c.
//
// A TupleTableSlot is the executor's universal tuple container. It holds
// a tuple in deformed (Datum array) form and optionally owns the backing
// HeapTuple. The slot is the primary vehicle for passing tuples between
// plan nodes.
#include "pgcpp/executor/tupletable.hpp"

#include <new>

#include "pgcpp/access/heapam.hpp"
#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/common/memory/memory_context.hpp"

namespace pgcpp::executor {
using pgcpp::nodes::makePallocNode;

using pgcpp::access::TupleDesc;
using pgcpp::memory::palloc;
using pgcpp::memory::pfree;
using pgcpp::transaction::HeapTuple;
using pgcpp::types::Datum;

TupleTableSlot* TupleTableSlot::Make(TupleDesc tupdesc) {
    auto* slot = makePallocNode<TupleTableSlot>();
    slot->tts_tupleDescriptor = tupdesc;
    int natts = tupdesc != nullptr ? tupdesc->natts : 0;
    if (natts > 0) {
        slot->tts_values = static_cast<Datum*>(palloc(sizeof(Datum) * natts));
        slot->tts_isnull = static_cast<bool*>(palloc(sizeof(bool) * natts));
        for (int i = 0; i < natts; i++) {
            slot->tts_values[i] = 0;
            slot->tts_isnull[i] = true;
        }
    }
    slot->tts_nvalid = false;
    slot->tts_isempty = true;
    return slot;
}

void TupleTableSlot::StoreTuple(HeapTuple tuple, bool shouldFree) {
    // Free any previously-stored tuple if we own it.
    if (tts_shouldFree && tts_tuple != nullptr) {
        pgcpp::access::heap_freetuple(tts_tuple);
    }
    tts_tuple = tuple;
    tts_shouldFree = shouldFree;
    if (tuple != nullptr && tts_tupleDescriptor != nullptr) {
        pgcpp::access::heap_deform_tuple(tuple, tts_tupleDescriptor, tts_values, tts_isnull);
        tts_nvalid = true;
        tts_isempty = false;
    } else {
        tts_nvalid = false;
        tts_isempty = (tuple == nullptr);
    }
}

void TupleTableSlot::StoreVirtual(const Datum* values, const bool* isnull) {
    // Free any previously-stored physical tuple if we own it.
    if (tts_shouldFree && tts_tuple != nullptr) {
        pgcpp::access::heap_freetuple(tts_tuple);
    }
    tts_tuple = nullptr;
    tts_shouldFree = false;
    int natts = Natts();
    for (int i = 0; i < natts; i++) {
        tts_values[i] = values != nullptr ? values[i] : 0;
        tts_isnull[i] = isnull != nullptr ? isnull[i] : true;
    }
    tts_nvalid = true;
    tts_isempty = false;
}

void TupleTableSlot::Clear() {
    if (tts_shouldFree && tts_tuple != nullptr) {
        pgcpp::access::heap_freetuple(tts_tuple);
    }
    tts_tuple = nullptr;
    tts_shouldFree = false;
    int natts = Natts();
    for (int i = 0; i < natts; i++) {
        tts_values[i] = 0;
        tts_isnull[i] = true;
    }
    tts_nvalid = false;
    tts_isempty = true;
}

TupleTableSlot::~TupleTableSlot() {
    if (tts_shouldFree && tts_tuple != nullptr) {
        pgcpp::access::heap_freetuple(tts_tuple);
    }
    if (tts_values != nullptr) {
        pfree(tts_values);
    }
    if (tts_isnull != nullptr) {
        pfree(tts_isnull);
    }
}

}  // namespace pgcpp::executor

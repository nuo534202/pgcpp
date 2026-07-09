// node_bitmap_heapscan.h — BitmapHeapScan node state.
//
// Fetches heap tuples by the TID bitmap produced by a BitmapIndexScan child.
// The lefttree must be a BitmapIndexScan. On the first ExecProcNode call we
// drive the child once to build its TID vector, then iterate over the TIDs,
// fetch each heap tuple, apply the residual qual, and project the target
// list. A simple sequential heap scan per TID is used (matching the
// simplified heap_fetch path in node_indexscan.cpp).
#pragma once

#include <vector>

#include "access/heapam.hpp"
#include "access/rel.hpp"
#include "executor/node_exec.hpp"
#include "transaction/heap_tuple.hpp"

namespace pgcpp::executor {

class BitmapHeapScanState : public PlanState {
public:
    BitmapHeapScanState(Plan* p, EState* s) : PlanState(p, s) {}

    pgcpp::access::Relation bhss_relation = nullptr;
    TupleTableSlot* bhss_ScanTupleSlot = nullptr;

    // TID vector fetched from the BitmapIndexScan child.
    std::vector<pgcpp::transaction::ItemPointerData> tids;
    size_t tid_index = 0;
    bool bitmap_fetched = false;

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;
};

}  // namespace pgcpp::executor

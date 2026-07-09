// node_bitmap_indexscan.h — BitmapIndexScan node state.
//
// Scans a B-tree index using the index quals and collects all matching TIDs
// into a shared vector (the "bitmap"). This node produces no tuples itself;
// its output is the TID list, which is consumed by the parent BitmapHeapScan.
//
// The TID vector is owned by the BitmapIndexScanState and exposed via a
// pointer so the parent can read it after the child has been drained. The
// parent is responsible for not reading the vector before the first call
// to ExecProcNode() on this node has populated it.
#pragma once

#include <vector>

#include "access/nbtree.hpp"
#include "access/rel.hpp"
#include "executor/node_exec.hpp"
#include "transaction/heap_tuple.hpp"

namespace pgcpp::executor {

class BitmapIndexScanState : public PlanState {
public:
    BitmapIndexScanState(Plan* p, EState* s) : PlanState(p, s) {}

    pgcpp::access::Relation biss_relation = nullptr;  // heap relation (for qual eval)
    pgcpp::access::Relation biss_index = nullptr;     // index relation
    pgcpp::access::BTScanDesc biss_scanDesc = nullptr;

    // The collected TID bitmap. Populated lazily on the first ExecProcNode
    // call; subsequent calls return nullptr.
    std::vector<pgcpp::transaction::ItemPointerData> tids;
    bool bitmap_built = false;

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;
};

}  // namespace pgcpp::executor

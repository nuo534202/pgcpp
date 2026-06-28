// node_material.h — Material node state (cache child output in memory).
//
// On first scan, drains the child into an in-memory tuple store.
// Subsequent rescans replay the stored tuples without re-executing
// the child.
#pragma once

#include <vector>

#include "mytoydb/executor/node_exec.hpp"

namespace mytoydb::executor {

class MaterialState : public PlanState {
public:
    MaterialState(Plan* p, EState* s) : PlanState(p, s) {}

    std::vector<TupleTableSlot*> mat_tuples;  // cached child tuples
    bool mat_done = false;                    // child fully drained?
    size_t mat_index = 0;                     // next tuple to output

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;
};

}  // namespace mytoydb::executor

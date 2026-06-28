// node_subqueryscan.h — SubqueryScan node state (FROM subquery).
//
// Wraps a subquery's plan (stored as lefttree) and projects its
// output through the parent query's target list.
#pragma once

#include "mytoydb/executor/node_exec.hpp"

namespace mytoydb::executor {

class SubqueryScanState : public PlanState {
public:
    SubqueryScanState(Plan* p, EState* s) : PlanState(p, s) {}

    TupleTableSlot* ss_ScanTupleSlot = nullptr;  // child's output slot

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;
};

}  // namespace mytoydb::executor

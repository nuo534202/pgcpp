// node_foreignscan.h — ForeignScan node state (FDW scan).
//
// Converted from PostgreSQL 15's src/include/nodes/execnodes.h (ForeignScanState)
// and src/backend/executor/nodeForeignscan.c.
//
// ForeignScanState is the executor runtime state for a ForeignScan plan node.
// It holds a pointer to the FDW routine (callback table) and the FDW handler's
// private state. The executor calls the FDW callbacks to drive the scan:
//   BeginForeignScan → IterateForeignScan (repeated) → EndForeignScan
//
// The FDW handler fills fs_ScanTupleSlot with each foreign row. The executor
// then applies the qual filter and target-list projection, producing the
// output in ps_ResultTupleSlot (inherited from PlanState).
#pragma once

#include "executor/node_exec.hpp"
#include "foreign/fdwapi.hpp"

namespace pgcpp::executor {

class ForeignScanState : public PlanState {
public:
    ForeignScanState(Plan* p, EState* s) : PlanState(p, s) {}

    // The FDW routine (callback table). Resolved in ExecInit from the foreign
    // table's server → fdwname → LookupFdw().
    const pgcpp::foreign::FdwRoutine* fs_routine = nullptr;

    // FDW-private state. The handler allocates this in BeginForeignScan and
    // frees it in EndForeignScan. The executor never touches its contents.
    void* fdw_state = nullptr;

    // The scan tuple slot — filled by the FDW handler with each foreign row.
    // Its TupleDesc matches the foreign table's schema (from pg_attribute).
    TupleTableSlot* fs_ScanTupleSlot = nullptr;

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;
};

}  // namespace pgcpp::executor

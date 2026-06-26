// node_seqscan.h — Sequential scan node state.
#pragma once

#include "mytoydb/access/heapam.hpp"
#include "mytoydb/access/rel.hpp"
#include "mytoydb/executor/node_exec.hpp"

namespace mytoydb::executor {

class SeqScanState : public PlanState {
public:
    SeqScanState(Plan* p, EState* s) : PlanState(p, s) {}

    mytoydb::access::Relation ss_relation = nullptr;
    mytoydb::access::HeapScanDesc ss_scanDesc = nullptr;
    TupleTableSlot* ss_ScanTupleSlot = nullptr;

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;
};

}  // namespace mytoydb::executor

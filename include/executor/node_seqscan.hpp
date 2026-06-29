// node_seqscan.h — Sequential scan node state.
#pragma once

#include "access/heapam.hpp"
#include "access/rel.hpp"
#include "executor/node_exec.hpp"

namespace pgcpp::executor {

class SeqScanState : public PlanState {
public:
    SeqScanState(Plan* p, EState* s) : PlanState(p, s) {}

    pgcpp::access::Relation ss_relation = nullptr;
    pgcpp::access::HeapScanDesc ss_scanDesc = nullptr;
    TupleTableSlot* ss_ScanTupleSlot = nullptr;

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;
};

}  // namespace pgcpp::executor

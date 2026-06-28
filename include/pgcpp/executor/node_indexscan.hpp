// node_indexscan.h — Index scan node state.
#pragma once

#include "mytoydb/access/nbtree.hpp"
#include "mytoydb/access/rel.hpp"
#include "mytoydb/executor/node_exec.hpp"

namespace mytoydb::executor {

class IndexScanState : public PlanState {
public:
    IndexScanState(Plan* p, EState* s) : PlanState(p, s) {}

    mytoydb::access::Relation iss_relation = nullptr;
    mytoydb::access::Relation iss_index = nullptr;
    mytoydb::access::BTScanDesc iss_scanDesc = nullptr;
    TupleTableSlot* iss_ScanTupleSlot = nullptr;

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;
};

}  // namespace mytoydb::executor

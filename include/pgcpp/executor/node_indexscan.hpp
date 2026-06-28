// node_indexscan.h — Index scan node state.
#pragma once

#include "pgcpp/access/nbtree.hpp"
#include "pgcpp/access/rel.hpp"
#include "pgcpp/executor/node_exec.hpp"

namespace pgcpp::executor {

class IndexScanState : public PlanState {
public:
    IndexScanState(Plan* p, EState* s) : PlanState(p, s) {}

    pgcpp::access::Relation iss_relation = nullptr;
    pgcpp::access::Relation iss_index = nullptr;
    pgcpp::access::BTScanDesc iss_scanDesc = nullptr;
    TupleTableSlot* iss_ScanTupleSlot = nullptr;

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;
};

}  // namespace pgcpp::executor

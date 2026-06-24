// node_modify_table.h — DML node state (INSERT/UPDATE/DELETE).
#pragma once

#include "mytoydb/access/heapam.h"
#include "mytoydb/access/rel.h"
#include "mytoydb/executor/node_exec.h"

namespace mytoydb::executor {

class ModifyTableState : public PlanState {
public:
    ModifyTableState(Plan* p, EState* s) : PlanState(p, s) {}

    mytoydb::parser::CmdType mt_operation = mytoydb::parser::CmdType::kInsert;
    mytoydb::access::Relation mt_relation = nullptr;
    mytoydb::access::TupleDesc mt_tupDesc = nullptr;

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;
};

}  // namespace mytoydb::executor

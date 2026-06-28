// node_modify_table.h — DML node state (INSERT/UPDATE/DELETE).
#pragma once

#include "pgcpp/access/heapam.hpp"
#include "pgcpp/access/rel.hpp"
#include "pgcpp/executor/node_exec.hpp"

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

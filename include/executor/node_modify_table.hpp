// node_modify_table.h — DML node state (INSERT/UPDATE/DELETE).
#pragma once

#include "access/heapam.hpp"
#include "access/rel.hpp"
#include "executor/node_exec.hpp"

namespace pgcpp::executor {

class ModifyTableState : public PlanState {
public:
    ModifyTableState(Plan* p, EState* s) : PlanState(p, s) {}

    pgcpp::parser::CmdType mt_operation = pgcpp::parser::CmdType::kInsert;
    pgcpp::access::Relation mt_relation = nullptr;
    pgcpp::access::TupleDesc mt_tupDesc = nullptr;

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;
};

}  // namespace pgcpp::executor

// node_lockrows.h — LockRows executor node (SELECT FOR UPDATE/SHARE).
//
// Converted from PostgreSQL 15's src/include/executor/nodeLockRows.h.
//
// LockRows wraps a child plan and acquires a row-level lock on each tuple
// produced by the child. The lock is recorded in the tuple header via
// heap_lock_tuple (setting t_xmax + infomask flags). In single-process
// pgcpp, the lock always succeeds (no blocking).
//
// The tuple is passed through to the parent unchanged — LockRows does not
// modify the tuple data, only the underlying heap tuple's MVCC metadata.
#pragma once

#include "access/rel.hpp"
#include "executor/node_exec.hpp"
#include "transaction/lock.hpp"

namespace pgcpp::executor {

class LockRowsState : public PlanState {
public:
    LockRowsState(Plan* p, EState* s) : PlanState(p, s) {}

    pgcpp::access::Relation lr_relation = nullptr;
    pgcpp::transaction::RowLockStrength lr_lockStrength =
        pgcpp::transaction::RowLockStrength::kForUpdate;

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;
};

}  // namespace pgcpp::executor

// node_unique.h — Unique node state (SELECT DISTINCT).
//
// Deduplicates a sorted input. Assumes the child produces tuples
// already sorted on uniq_colIdx columns so duplicates are adjacent.
// Returns a tuple only when it differs from the previously returned one.
#pragma once

#include <vector>

#include "pgcpp/executor/node_exec.hpp"

namespace pgcpp::executor {

class UniqueState : public PlanState {
public:
    UniqueState(Plan* p, EState* s) : PlanState(p, s) {}

    std::vector<int> uniq_colIdx;          // 1-based attr numbers to compare
    TupleTableSlot* last_tuple = nullptr;  // previously returned tuple
    bool has_last = false;                 // has last_tuple been set?

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;
};

}  // namespace pgcpp::executor

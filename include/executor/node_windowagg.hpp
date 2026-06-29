// node_windowagg.h — WindowAgg node state (window functions / OVER clause).
//
// Computes window aggregates over partitions. Assumes the child is
// sorted by PARTITION BY then ORDER BY columns so that rows in the
// same partition are contiguous. For each partition, computes running
// aggregates (e.g., SUM, COUNT, ROW_NUMBER) and projects the result.
#pragma once

#include <vector>

#include "executor/node_agg.hpp"  // AggState for per-agg state reuse
#include "executor/node_exec.hpp"

namespace pgcpp::executor {

class WindowAggState : public PlanState {
public:
    WindowAggState(Plan* p, EState* s) : PlanState(p, s) {}

    std::vector<int> wa_partColIdx;   // PARTITION BY attr numbers (1-based)
    std::vector<int> wa_ordColIdx;    // ORDER BY attr numbers (1-based)
    std::vector<bool> wa_ordReverse;  // DESC per ORDER BY column

    // Cached child tuples (drained on first call).
    std::vector<TupleTableSlot*> wa_tuples;
    bool wa_drained = false;
    size_t wa_output_index = 0;

    // Per-aggregate metadata (one entry per Aggref in the target list).
    std::vector<AggStateInfo> wa_agg_infos;
    // Running aggregate state for the current partition. Reused across
    // partitions: reset when the partition key changes.
    AggGroupState wa_running;
    bool wa_running_init = false;
    // Slot holding the current row's aggregate values, used by ExecProject.
    TupleTableSlot* wa_AggSlot = nullptr;
    // Last partition-key tuple (for detecting partition changes).
    std::vector<pgcpp::types::Datum> wa_last_part_key;
    std::vector<bool> wa_last_part_null;
    bool wa_has_last_part = false;

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;

private:
    void CollectAggrefs();
    void InitRunningState();
    void AccumulateRow(TupleTableSlot* row);
    void ResetForNewPartition();
    bool SamePartition(TupleTableSlot* row);
    void StoreAggregates();
};

}  // namespace pgcpp::executor

// node_projectset.h — ProjectSet node state.
//
// Projects a target list containing set-returning functions. For each input
// tuple (from lefttree), SRFs may produce multiple values; the node returns
// one output row per SRF value. Non-SRF target entries repeat their value.
#pragma once

#include <vector>

#include "executor/node_exec.hpp"

namespace pgcpp::executor {

class ProjectSetState : public PlanState {
public:
    ProjectSetState(Plan* p, EState* s) : PlanState(p, s) {}

    // For each SRF target entry, the queue of values not yet emitted for the
    // current input tuple. Non-SRF entries have empty queues and reuse the
    // scalar value computed for the current input tuple.
    struct SrfQueue {
        std::vector<pgcpp::types::Datum> values;
        std::vector<bool> isnull;
        size_t pos = 0;
        bool is_srf = false;
        pgcpp::types::Datum scalar_value = 0;
        bool scalar_isnull = true;
    };
    std::vector<SrfQueue> ps_queues;
    bool ps_done = false;  // no more input tuples and queues drained

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;
};

}  // namespace pgcpp::executor

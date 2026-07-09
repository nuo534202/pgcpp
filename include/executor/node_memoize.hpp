// node_memoize.h — Memoize node state (parameterized inner-side cache).
//
// Caches the child plan's output keyed by parameter values (evaluated from
// param_exprs in the outer tuple's context). On a key hit, cached rows are
// replayed without re-executing the child; on a miss, the child is drained
// and its output stored under the new key.
#pragma once

#include <map>
#include <vector>

#include "executor/node_exec.hpp"
#include "types/datum.hpp"

namespace pgcpp::executor {

class MemoizeState : public PlanState {
public:
    MemoizeState(Plan* p, EState* s) : PlanState(p, s) {}

    // Cached entry: rows produced for one key.
    struct CacheEntry {
        std::vector<TupleTableSlot*> rows;
        size_t read_pos = 0;
    };

    // Key = concatenation of the param datums (by-value only, simplification).
    std::map<std::vector<pgcpp::types::Datum>, CacheEntry> ms_cache;

    // Iterator to the current cache entry (end() = no active key).
    std::map<std::vector<pgcpp::types::Datum>, CacheEntry>::iterator ms_cur = ms_cache.end();
    bool ms_first_call = true;

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;
};

}  // namespace pgcpp::executor

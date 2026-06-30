// node_agg.h — Aggregate node state.
//
// Supports COUNT, SUM, AVG, MIN, MAX with optional GROUP BY.
// Aggregates are identified by Aggref nodes in the target list;
// each Aggref's aggno indexes into the per-group aggregate state array.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "executor/node_exec.hpp"
#include "types/datum.hpp"

namespace pgcpp::executor {

// Aggregate function kinds (identified by aggfnoid).
enum class AggKind {
    kCount,  // COUNT(*) or COUNT(expr)
    kSum,    // SUM(expr)
    kAvg,    // AVG(expr)
    kMin,    // MIN(expr)
    kMax,    // MAX(expr)
};

// AggStateInfo — per-aggregate runtime info.
struct AggStateInfo {
    AggKind kind = AggKind::kCount;
    pgcpp::catalog::Oid argtype = 0;     // type of the aggregate's argument
    pgcpp::catalog::Oid restype = 0;     // type of the aggregate's result
    int aggno = -1;                      // index into the state arrays
    bool isstar = false;                 // COUNT(*) vs COUNT(expr)
    bool distinct = false;               // DISTINCT flag (Aggref::aggdistinct non-empty)
    pgcpp::parser::Node* arg = nullptr;  // argument expression (nullptr for COUNT(*))
};

// AggGroupKey — a GROUP BY key (vector of Datum values, hashed).
//
// For pass-by-reference types (text, varchar), the Datum is a pointer to a
// varlena buffer. Two rows with the same string content but different buffer
// addresses must hash and compare equal, so by-reference columns are
// serialized into `str_values` at key-construction time. For pass-by-value
// types, `str_values[i]` is left empty and the Datum in `values[i]` is used
// directly.
struct AggGroupKey {
    std::vector<pgcpp::types::Datum> values;
    std::vector<std::string> str_values;
    std::vector<bool> isnull;
    bool operator==(const AggGroupKey& o) const {
        return values == o.values && str_values == o.str_values && isnull == o.isnull;
    }
};

struct AggGroupKeyHash {
    size_t operator()(const AggGroupKey& k) const {
        size_t h = 0;
        for (size_t i = 0; i < k.values.size(); i++) {
            h ^= std::hash<uintptr_t>{}(k.values[i]) + 0x9e3779b9u + (h << 6) + (h >> 2);
            h ^= std::hash<bool>{}(k.isnull[i]) + 0x9e3779b9u + (h << 6) + (h >> 2);
        }
        for (size_t i = 0; i < k.str_values.size(); i++) {
            h ^= std::hash<std::string>{}(k.str_values[i]) + 0x9e3779b9u + (h << 6) + (h >> 2);
        }
        return h;
    }
};

// DistinctSet — per-aggregate set of seen values for DISTINCT aggregates.
// Pass-by-value types (int2/4/8, float4/8, date, timestamp) use the Datum
// bits directly via `int_values`. Pass-by-reference types (text, varchar)
// use the serialized string content via `str_values` so that two rows with
// identical text at different buffer addresses compare equal.
struct DistinctSet {
    std::unordered_set<int64_t> int_values;
    std::unordered_set<std::string> str_values;
};

// AggGroupState — per-group aggregate state.
struct AggGroupState {
    std::vector<int64_t> count;                // count per aggregate
    std::vector<int64_t> sum_int;              // integer sum per aggregate
    std::vector<double> sum_float;             // float sum per aggregate
    std::vector<pgcpp::types::Datum> min_val;  // min per aggregate
    std::vector<pgcpp::types::Datum> max_val;  // max per aggregate
    std::vector<bool> minmax_init;             // has min/max been initialized?
    std::vector<bool> minmax_null;             // is min/max currently null?
    std::vector<DistinctSet> distinct_sets;    // per-aggregate seen-values set
};

class AggState : public PlanState {
public:
    AggState(Plan* p, EState* s) : PlanState(p, s) {}

    std::vector<AggStateInfo> agg_infos;  // one per Aggref in target list
    bool has_group_by = false;
    std::vector<int> group_col_idx;     // 1-based attr numbers of GROUP BY cols
    std::vector<bool> group_col_byref;  // parallel to group_col_idx; true if pass-by-reference

    // Hash table for grouped aggregation.
    std::unordered_map<AggGroupKey, AggGroupState, AggGroupKeyHash> groups;
    // For plain aggregation (no GROUP BY), a single group.
    AggGroupState plain_group;
    bool plain_group_init = false;

    // Iterator for outputting groups.
    typename std::unordered_map<AggGroupKey, AggGroupState, AggGroupKeyHash>::iterator group_iter;
    bool output_started = false;

    void ExecInit() override;
    TupleTableSlot* ExecProcNode() override;
    void ExecEnd() override;
    void ExecReScan() override;

private:
    void CollectAggrefs();
    void InitGroupState(AggGroupState& gs);
    void Accumulate(AggGroupState& gs, ExprContext* econtext);
    TupleTableSlot* BuildOutputSlot(const AggGroupKey& key, const AggGroupState& gs);
};

}  // namespace pgcpp::executor

// costsize.cpp — Cost estimation for the optimizer.
//
// Converted from PostgreSQL 15's src/backend/optimizer/path/costsize.c.
//
// Estimates the execution cost of plan operations using simple heuristics.
// The cost model mirrors PostgreSQL's: I/O cost (page fetches) plus CPU cost
// (tuple processing + operator evaluation).
#include <algorithm>
#include <cmath>

#include "mytoydb/common/containers/node.hpp"
#include "mytoydb/optimizer/cost.hpp"
#include "mytoydb/parser/primnodes.hpp"

namespace mytoydb::optimizer {

using mytoydb::nodes::NodeTag;
using mytoydb::parser::BoolExpr;
using mytoydb::parser::BoolExprType;
using mytoydb::parser::Node;
using mytoydb::parser::OpExpr;
using mytoydb::parser::Var;

void CostSeqScan(SeqScanPath* path, int pages, int tuples) {
    // Cost = I/O cost (pages * seq_page_cost) + CPU cost (tuples * cpu_tuple_cost)
    Cost startup = 0.0;
    Cost run_cost =
        static_cast<Cost>(pages) * kSeqPageCost + static_cast<Cost>(tuples) * kCpuTupleCost;
    path->startup_cost = startup;
    path->total_cost = startup + run_cost;
    path->rows = static_cast<Cardinality>(tuples);
}

void CostIndexScan(IndexPath* path, int tuples, Selectivity selectivity) {
    // Estimate number of tuples fetched via index.
    int fetched = std::max(1, static_cast<int>(tuples * selectivity));
    // I/O cost: random page fetches for each tuple (heuristic).
    Cost io_cost = static_cast<Cost>(fetched) * kRandomPageCost;
    // CPU cost: index tuple processing + heap tuple processing.
    Cost cpu_cost = static_cast<Cost>(fetched) * kCpuIndexTupleCost +
                    static_cast<Cost>(fetched) * kCpuTupleCost;
    path->startup_cost = 0.0;
    path->total_cost = io_cost + cpu_cost;
    path->rows = static_cast<Cardinality>(fetched);
}

Cost CostSort(int tuples, int width, int64_t limit) {
    if (tuples <= 1)
        return 0.0;
    int n = (limit > 0 && limit < tuples) ? static_cast<int>(limit) : tuples;
    // Comparison cost: O(n log n) comparisons, each costing operator_cost.
    double log_n = std::log2(static_cast<double>(n));
    return static_cast<Cost>(n) * log_n * kOperatorCost;
}

Cost CostAgg(int input_rows, int num_groups, int width) {
    // Cost: one transition per input row + one finalization per group.
    Cost transition_cost = static_cast<Cost>(input_rows) * kOperatorCost;
    Cost finalization_cost = static_cast<Cost>(num_groups) * kCpuTupleCost;
    return transition_cost + finalization_cost;
}

Cardinality ClampRowEst(Cardinality rows) {
    if (rows < 1.0)
        return 1.0;
    return rows;
}

// Estimate selectivity of a qual expression.
// For simple equality (col = const), returns 1/ndistinct (heuristic: 0.1).
// For range (col < const, col > const), returns 0.33.
// For AND, multiplies selectivities.
// For OR, uses 1 - (1-s1)*(1-s2).
// Default: 0.5 (unknown).
Selectivity EstimateSelectivity(const Node* qual, int total_rows) {
    if (qual == nullptr)
        return 1.0;

    NodeTag tag = qual->GetTag();

    if (tag == NodeTag::kOpExpr) {
        const auto* op = static_cast<const OpExpr*>(qual);
        if (op->args.size() != 2)
            return 0.5;

        // Check if one side is a Var and the other is a Const.
        bool left_is_var = (op->args[0] != nullptr && op->args[0]->GetTag() == NodeTag::kVar);
        bool right_is_const = (op->args[1] != nullptr && op->args[1]->GetTag() == NodeTag::kConst);

        if (left_is_var && right_is_const) {
            // Look up operator name from catalog to determine selectivity.
            // For equality (=): selectivity ~ 1/ndistinct (heuristic: 0.1).
            // For range (<, >, <=, >=): selectivity ~ 0.33.
            // We use the opno to guess: even-numbered ops tend to be comparison.
            // A more accurate approach would look up the operator name.
            // For now, use a simple heuristic based on opno ranges.
            int opno = op->opno;
            // PostgreSQL operator OIDs: 96 (=), 97 (<), 521 (>), etc.
            if (opno == 96)
                return 0.1;  // int4eq
            return 0.33;     // range comparison
        }
        return 0.5;
    }

    if (tag == NodeTag::kBoolExpr) {
        const auto* boolexpr = static_cast<const BoolExpr*>(qual);
        if (boolexpr->args.empty())
            return 0.5;

        if (boolexpr->boolop == BoolExprType::kAnd) {
            Selectivity result = 1.0;
            for (const Node* arg : boolexpr->args) {
                result *= EstimateSelectivity(arg, total_rows);
            }
            return result;
        }
        if (boolexpr->boolop == BoolExprType::kOr) {
            Selectivity not_match = 1.0;
            for (const Node* arg : boolexpr->args) {
                not_match *= (1.0 - EstimateSelectivity(arg, total_rows));
            }
            return 1.0 - not_match;
        }
        // NOT
        return 1.0 - EstimateSelectivity(boolexpr->args[0], total_rows);
    }

    return 0.5;
}

}  // namespace mytoydb::optimizer

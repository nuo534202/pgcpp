// costsize.cpp — Cost estimation for the optimizer.
//
// Converted from PostgreSQL 15's src/backend/optimizer/path/costsize.c.
//
// Estimates the execution cost of plan operations using simple heuristics.
// The cost model mirrors PostgreSQL's: I/O cost (page fetches) plus CPU cost
// (tuple processing + operator evaluation).
//
// Selectivity estimation uses pg_statistic when available (MCV for equality,
// histogram for range) and falls back to heuristics when no stats exist.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "catalog/catalog.hpp"
#include "catalog/pg_statistic.hpp"
#include "common/containers/node.hpp"
#include "optimizer/cost.hpp"
#include "parser/primnodes.hpp"
#include "types/datum.hpp"

namespace pgcpp::optimizer {

using pgcpp::catalog::FormData_pg_statistic;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::kInvalidOid;
using pgcpp::catalog::kStatisticKindHistogram;
using pgcpp::catalog::kStatisticKindMcv;
using pgcpp::catalog::Oid;
using pgcpp::nodes::NodeTag;
using pgcpp::parser::BoolExpr;
using pgcpp::parser::BoolExprType;
using pgcpp::parser::Const;
using pgcpp::parser::Node;
using pgcpp::parser::OpExpr;
using pgcpp::parser::Var;

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

Cost CostSort(int tuples, [[maybe_unused]] int width, int64_t limit) {
    if (tuples <= 1)
        return 0.0;
    int n = (limit > 0 && limit < tuples) ? static_cast<int>(limit) : tuples;
    // Comparison cost: O(n log n) comparisons, each costing operator_cost.
    double log_n = std::log2(static_cast<double>(n));
    return static_cast<Cost>(n) * log_n * kOperatorCost;
}

Cost CostAgg(int input_rows, int num_groups, [[maybe_unused]] int width) {
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

// --- Stats-based selectivity helpers ---

namespace {

// Parse the MCV string "v1:f1,v2:f2,..." for an int4 column.
// Returns true if the const_value is found in MCV; sets *freq_out.
bool McvLookupInt(const std::string& stavalues, int32_t const_value, float* freq_out) {
    if (stavalues.empty())
        return false;
    size_t pos = 0;
    while (pos < stavalues.size()) {
        size_t colon = stavalues.find(':', pos);
        if (colon == std::string::npos)
            return false;
        int32_t val = std::atoi(stavalues.substr(pos, colon - pos).c_str());
        size_t comma = stavalues.find(',', colon);
        std::string freq_str;
        if (comma == std::string::npos) {
            freq_str = stavalues.substr(colon + 1);
            pos = stavalues.size();
        } else {
            freq_str = stavalues.substr(colon + 1, comma - colon - 1);
            pos = comma + 1;
        }
        if (val == const_value) {
            *freq_out = static_cast<float>(std::atof(freq_str.c_str()));
            return true;
        }
    }
    return false;
}

// Parse the histogram string "v1,v2,v3,..." for an int4 column.
// Returns the fraction of values < const_value (for < and <=).
// If const_value is below the first bucket, returns 0.0.
// If above the last bucket, returns 1.0.
// Otherwise, interpolates linearly within the bucket.
Selectivity HistogramSelectivityInt(const std::string& stavalues, int32_t const_value, bool is_lt) {
    if (stavalues.empty())
        return -1.0;  // no histogram available

    std::vector<int32_t> buckets;
    size_t pos = 0;
    while (pos < stavalues.size()) {
        size_t comma = stavalues.find(',', pos);
        std::string val_str;
        if (comma == std::string::npos) {
            val_str = stavalues.substr(pos);
            pos = stavalues.size();
        } else {
            val_str = stavalues.substr(pos, comma - pos);
            pos = comma + 1;
        }
        buckets.push_back(std::atoi(val_str.c_str()));
    }
    if (buckets.empty())
        return -1.0;

    int n = static_cast<int>(buckets.size());
    if (n == 1) {
        if (const_value < buckets[0])
            return is_lt ? 0.0 : 1.0;
        return is_lt ? 1.0 : 0.0;
    }

    // Find the bucket containing const_value.
    if (const_value <= buckets[0]) {
        return is_lt ? 0.0 : 1.0;
    }
    if (const_value >= buckets[n - 1]) {
        return is_lt ? 1.0 : 0.0;
    }

    // Linear interpolation within the bucket.
    for (int i = 0; i < n - 1; i++) {
        if (const_value >= buckets[i] && const_value < buckets[i + 1]) {
            double range = static_cast<double>(buckets[i + 1] - buckets[i]);
            if (range == 0)
                range = 1.0;
            double frac = static_cast<double>(const_value - buckets[i]) / range;
            // Fraction of values below const_value.
            double below = (static_cast<double>(i) + frac) / n;
            if (is_lt)
                return static_cast<Selectivity>(below);
            else
                return static_cast<Selectivity>(1.0 - below);
        }
    }
    return 0.5;
}

// Compute total MCV frequency and MCV contribution for range selectivity.
// For col < const: sums frequencies of MCV values < const.
// For col > const: sums frequencies of MCV values > const.
// Also returns total MCV frequency (sum of all MCV frequencies).
float McvRangeContributionInt(const std::string& stavalues, int32_t const_value, bool is_lt,
                              float* mcv_total_freq) {
    *mcv_total_freq = 0.0F;
    if (stavalues.empty())
        return 0.0F;

    float contribution = 0.0F;
    size_t pos = 0;
    while (pos < stavalues.size()) {
        size_t colon = stavalues.find(':', pos);
        if (colon == std::string::npos)
            break;
        int32_t val = std::atoi(stavalues.substr(pos, colon - pos).c_str());
        size_t comma = stavalues.find(',', colon);
        std::string freq_str;
        if (comma == std::string::npos) {
            freq_str = stavalues.substr(colon + 1);
            pos = stavalues.size();
        } else {
            freq_str = stavalues.substr(colon + 1, comma - colon - 1);
            pos = comma + 1;
        }
        float freq = static_cast<float>(std::atof(freq_str.c_str()));
        *mcv_total_freq += freq;
        if (is_lt) {
            if (val < const_value)
                contribution += freq;
        } else {
            if (val > const_value)
                contribution += freq;
        }
    }
    return contribution;
}

// Compute eqsel (col = const) selectivity from pg_statistic.
Selectivity EqSelectivity(const FormData_pg_statistic* stat, int32_t const_value) {
    if (stat == nullptr)
        return 0.1;  // default heuristic

    // Try MCV first.
    if (stat->stakind1 == kStatisticKindMcv) {
        float freq = 0.0F;
        if (McvLookupInt(stat->stavalues1, const_value, &freq))
            return static_cast<Selectivity>(freq);
    }

    // Not in MCV: use 1/ndistinct.
    if (stat->stadistinct > 0)
        return static_cast<Selectivity>(1.0 / stat->stadistinct);

    return 0.1;  // default heuristic
}

// Compute scalarltsel (col < const) selectivity from pg_statistic.
// Combines MCV contribution (values < const) with histogram (non-MCV portion).
Selectivity LtSelectivity(const FormData_pg_statistic* stat, int32_t const_value) {
    if (stat == nullptr)
        return 0.33;

    // MCV contribution: sum frequencies of MCV values < const.
    float mcv_total = 0.0F;
    float mcv_below = 0.0F;
    if (stat->stakind1 == kStatisticKindMcv) {
        mcv_below = McvRangeContributionInt(stat->stavalues1, const_value, true, &mcv_total);
    }

    // Histogram contribution: fraction of non-MCV values < const.
    if (stat->stakind2 == kStatisticKindHistogram) {
        Selectivity hist_frac = HistogramSelectivityInt(stat->stavalues2, const_value, true);
        if (hist_frac >= 0.0) {
            // Histogram covers (1 - mcv_total) of rows.
            double non_mcv_frac = 1.0 - static_cast<double>(mcv_total);
            double result = static_cast<double>(mcv_below) + hist_frac * non_mcv_frac;
            return static_cast<Selectivity>(result);
        }
    }

    // No histogram: use MCV-only estimate or default.
    if (mcv_total > 0.0F)
        return static_cast<Selectivity>(mcv_below);
    return 0.33;
}

// Compute scalargtsel (col > const) selectivity from pg_statistic.
// Combines MCV contribution (values > const) with histogram (non-MCV portion).
Selectivity GtSelectivity(const FormData_pg_statistic* stat, int32_t const_value) {
    if (stat == nullptr)
        return 0.33;

    // MCV contribution: sum frequencies of MCV values > const.
    float mcv_total = 0.0F;
    float mcv_above = 0.0F;
    if (stat->stakind1 == kStatisticKindMcv) {
        mcv_above = McvRangeContributionInt(stat->stavalues1, const_value, false, &mcv_total);
    }

    // Histogram contribution: fraction of non-MCV values > const.
    if (stat->stakind2 == kStatisticKindHistogram) {
        Selectivity hist_frac = HistogramSelectivityInt(stat->stavalues2, const_value, false);
        if (hist_frac >= 0.0) {
            double non_mcv_frac = 1.0 - static_cast<double>(mcv_total);
            double result = static_cast<double>(mcv_above) + hist_frac * non_mcv_frac;
            return static_cast<Selectivity>(result);
        }
    }

    // No histogram: use MCV-only estimate or default.
    if (mcv_total > 0.0F)
        return static_cast<Selectivity>(mcv_above);
    return 0.33;
}

}  // namespace

// Estimate selectivity of a qual expression.
// For simple equality (col = const), uses MCV or 1/ndistinct from pg_statistic.
// For range (col < const, col > const), uses histogram from pg_statistic.
// For AND, multiplies selectivities.
// For OR, uses 1 - (1-s1)*(1-s2).
// Default: 0.5 (unknown).
Selectivity EstimateSelectivity(const Node* qual, int total_rows, Oid relid) {
    if (qual == nullptr)
        return 1.0;

    NodeTag tag = qual->GetTag();

    if (tag == NodeTag::kOpExpr) {
        const auto* op = static_cast<const OpExpr*>(qual);
        if (op->args.size() != 2)
            return 0.5;

        bool left_is_var = (op->args[0] != nullptr && op->args[0]->GetTag() == NodeTag::kVar);
        bool right_is_const = (op->args[1] != nullptr && op->args[1]->GetTag() == NodeTag::kConst);

        if (left_is_var && right_is_const) {
            const auto* var = static_cast<const Var*>(op->args[0]);
            const auto* con = static_cast<const Const*>(op->args[1]);
            int opno = op->opno;

            // Try to look up pg_statistic for this column.
            const FormData_pg_statistic* stat = nullptr;
            if (relid != kInvalidOid && GetCatalog() != nullptr) {
                stat = GetCatalog()->GetStatistic(relid, static_cast<int16_t>(var->varattno));
            }

            // Only int4 columns have stats-based selectivity in pgcpp.
            if (var->vartype == pgcpp::types::kInt4Oid &&
                con->consttype == pgcpp::types::kInt4Oid) {
                int32_t const_val = pgcpp::types::DatumGetInt32(con->constvalue);
                if (opno == 96)  // int4eq
                    return EqSelectivity(stat, const_val);
                if (opno == 97)  // int4lt
                    return LtSelectivity(stat, const_val);
                if (opno == 521)  // int4gt
                    return GtSelectivity(stat, const_val);
            }

            // Fallback: use heuristics for non-int4 or unknown operators.
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
                result *= EstimateSelectivity(arg, total_rows, relid);
            }
            return result;
        }
        if (boolexpr->boolop == BoolExprType::kOr) {
            Selectivity not_match = 1.0;
            for (const Node* arg : boolexpr->args) {
                not_match *= (1.0 - EstimateSelectivity(arg, total_rows, relid));
            }
            return 1.0 - not_match;
        }
        // NOT
        return 1.0 - EstimateSelectivity(boolexpr->args[0], total_rows, relid);
    }

    return 0.5;
}

}  // namespace pgcpp::optimizer

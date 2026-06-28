// partprune.cpp — partition pruning implementation.
//
// Converts the public API of PostgreSQL's src/backend/partitioning/partprune.c
// to C++20. PostgreSQL's pruning has two stages:
//   1. The planner (or executor) builds a list of PartitionPruneStep nodes
//      from restriction clauses on the partition key.
//   2. get_matching_partitions walks the steps, produces a Bitmapset of
//      surviving partition indexes, and returns it.
//
// We preserve the two-stage shape, replacing the Bitmapset with
// std::vector<int> for safe ownership. The algorithm:
//   - PARTITIONPRUNE_OP: evaluate the operator against every partition's
//     bound; keep the partitions whose bound could satisfy the predicate.
//   - PARTITIONPRUNE_COMBINE: recursively evaluate sub-steps; AND takes the
//     intersection, OR takes the union.
//
// Simplifications (called out in the project README):
//   - Datum comparison treats Datum as int64_t (see partbounds.cpp).
//   - HASH supports only kEqual pruning (inequality cannot prune under hash
//     partitioning); other operators yield "all partitions".
//   - NULL handling is limited: a NULL value in an op step matches only the
//     null-accepting partition (if any).

#include "pgcpp/partitioning/partprune.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "pgcpp/common/error/elog.hpp"

namespace pgcpp::partitioning {

namespace {

// Signed comparison on Datum using int32_t semantics (see partbounds.cpp
// for the rationale: Int32GetDatum stores int32 values as zero-extended
// uint32_t, so we truncate back to int32_t for comparison).
int CompareDatum(types::Datum a, types::Datum b) {
    int32_t ia = static_cast<int32_t>(a);
    int32_t ib = static_cast<int32_t>(b);
    if (ia < ib)
        return -1;
    if (ia > ib)
        return 1;
    return 0;
}

// True if "lhs OP rhs" holds under the integer interpretation.
bool OpHolds(PruneOp op, types::Datum lhs, types::Datum rhs) {
    switch (op) {
        case PruneOp::kLess:
            return CompareDatum(lhs, rhs) < 0;
        case PruneOp::kLessEqual:
            return CompareDatum(lhs, rhs) <= 0;
        case PruneOp::kEqual:
            return CompareDatum(lhs, rhs) == 0;
        case PruneOp::kGreaterEqual:
            return CompareDatum(lhs, rhs) >= 0;
        case PruneOp::kGreater:
            return CompareDatum(lhs, rhs) > 0;
    }
    return false;
}

// For LIST partitioning: does the partition's set of list values contain a
// value v such that "v OP step_value" holds?
bool ListPartitionMatchesOp(const PartitionBoundInfoData& bi, int partition_idx, PruneOp op,
                            types::Datum step_value) {
    for (std::size_t i = 0; i < bi.datums.size(); ++i) {
        if (bi.indexes[i] != partition_idx)
            continue;
        if (bi.datums[i].empty())
            continue;
        if (OpHolds(op, bi.datums[i][0], step_value)) {
            return true;
        }
    }
    return false;
}

// For RANGE partitioning: does the partition with bounds [lower, upper)
// contain at least one value v such that "v OP step_value" holds?
//
// The partition's range is half-open: lower <= v < upper. So:
//   kEqual:        lower <= step_value < upper
//   kLess:         lower <  step_value          (v = lower works if lower < step_value)
//   kLessEqual:    lower <= step_value          (v = lower works if lower <= step_value)
//   kGreaterEqual: upper >  step_value          (v = upper - 1 works if upper - 1 >= step_value)
//   kGreater:      upper >  step_value + 1      (v = upper - 1 works if upper - 1 > step_value)
bool RangePartitionMatchesOp(types::Datum lower, types::Datum upper, PruneOp op,
                             types::Datum step_value) {
    switch (op) {
        case PruneOp::kEqual:
            return CompareDatum(lower, step_value) <= 0 && CompareDatum(step_value, upper) < 0;
        case PruneOp::kLess:
            // The smallest value the partition can hold is `lower`. The
            // predicate "key < step_value" can be satisfied iff lower <
            // step_value (then v = lower is a witness).
            return CompareDatum(lower, step_value) < 0;
        case PruneOp::kLessEqual:
            return CompareDatum(lower, step_value) <= 0;
        case PruneOp::kGreaterEqual: {
            // The largest value the partition can hold is `upper - 1` (upper
            // is exclusive). Predicate "key >= step_value" is satisfiable
            // iff upper - 1 >= step_value, i.e. upper > step_value.
            int c = CompareDatum(upper, step_value);
            return c > 0;
        }
        case PruneOp::kGreater: {
            // Predicate "key > step_value" is satisfiable iff upper - 1 >
            // step_value, i.e. upper > step_value + 1. To avoid overflow we
            // check upper - step_value > 1 in signed-safe form via two
            // comparisons.
            int c = CompareDatum(upper, step_value);
            if (c <= 0)
                return false;
            // upper > step_value. Now check upper - 1 > step_value, i.e.
            // upper > step_value + 1. Equivalent to: NOT (upper ==
            // step_value + 1). Since upper != step_value here (c > 0), the
            // only way upper - 1 == step_value is upper == step_value + 1,
            // which means upper - step_value == 1. We detect that by
            // checking whether upper - 1 == step_value via subtraction that
            // is safe when upper > step_value (so upper >= step_value + 1,
            // upper - 1 >= step_value, no underflow). Use int32_t to match
            // the Int32GetDatum representation.
            int32_t iu = static_cast<int32_t>(upper);
            int32_t iv = static_cast<int32_t>(step_value);
            return (iu - 1) > iv;
        }
    }
    return false;
}

// Find the [lower, upper] pair for a given partition index in a RANGE
// boundinfo. Returns false if not found.
bool FindRangeBounds(const PartitionBoundInfoData& bi, int partition_idx, types::Datum* lower,
                     types::Datum* upper) {
    for (std::size_t i = 0; i < bi.datums.size(); ++i) {
        if (bi.indexes[i] != partition_idx)
            continue;
        if (bi.datums[i].size() < 2)
            continue;
        *lower = bi.datums[i][0];
        *upper = bi.datums[i][1];
        return true;
    }
    return false;
}

// Evaluate a single OP step against the boundinfo. Returns the surviving
// partition indexes (unsorted, deduplicated).
std::vector<int> EvaluateOpStep(const PruningContext& context, const PruneStepOp& step) {
    std::vector<int> result;
    const PartitionBoundInfoData* bi = context.boundinfo;
    if (bi == nullptr || context.nparts <= 0) {
        return result;
    }

    // NULL handling: only the null-accepting partition (or default) can
    // match a NULL predicate value.
    if (step.is_null) {
        if (bi->null_index >= 0 && bi->null_index < context.nparts) {
            result.push_back(bi->null_index);
        } else if (bi->default_index >= 0 && bi->default_index < context.nparts) {
            result.push_back(bi->default_index);
        }
        return result;
    }

    // HASH only supports equality pruning.
    if (context.strategy == PartitionStrategy::kHash) {
        if (step.op != PruneOp::kEqual) {
            // Cannot prune — return all partitions.
            result.reserve(static_cast<std::size_t>(context.nparts));
            for (int i = 0; i < context.nparts; ++i) {
                result.push_back(i);
            }
            return result;
        }
        int idx = partition_bound_accepts(*bi, step.value, /*is_null=*/false);
        if (idx >= 0 && idx < context.nparts) {
            result.push_back(idx);
        }
        return result;
    }

    // LIST and RANGE: iterate over each partition and decide.
    for (int i = 0; i < context.nparts; ++i) {
        bool match = false;
        if (context.strategy == PartitionStrategy::kList) {
            match = ListPartitionMatchesOp(*bi, i, step.op, step.value);
        } else if (context.strategy == PartitionStrategy::kRange) {
            types::Datum lower = 0;
            types::Datum upper = 0;
            if (FindRangeBounds(*bi, i, &lower, &upper)) {
                match = RangePartitionMatchesOp(lower, upper, step.op, step.value);
            }
        }
        if (match) {
            result.push_back(i);
        }
    }
    return result;
}

// Deduplicate and sort a partition index vector.
std::vector<int> Normalize(std::vector<int> in) {
    std::sort(in.begin(), in.end());
    in.erase(std::unique(in.begin(), in.end()), in.end());
    return in;
}

// Intersect two sorted, deduplicated vectors.
std::vector<int> Intersect(const std::vector<int>& a, const std::vector<int>& b) {
    std::vector<int> out;
    std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
    return out;
}

// Union two sorted, deduplicated vectors.
std::vector<int> Union(const std::vector<int>& a, const std::vector<int>& b) {
    std::vector<int> out;
    std::set_union(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
    return out;
}

// Recursive evaluator for an arbitrary step.
std::vector<int> EvaluateStep(const PruningContext& context, const PartitionPruneStep& step) {
    if (step.op_step != nullptr) {
        return Normalize(EvaluateOpStep(context, *step.op_step));
    }
    if (step.combine_step != nullptr) {
        std::vector<int> acc;
        bool first = true;
        for (const auto& sub : step.combine_step->sub_steps) {
            std::vector<int> sub_result = EvaluateStep(context, *sub);
            if (first) {
                acc = std::move(sub_result);
                first = false;
            } else {
                if (step.combine_step->combine_op == PruneCombineOp::kAnd) {
                    acc = Intersect(acc, sub_result);
                } else {
                    acc = Union(acc, sub_result);
                }
            }
        }
        // Special-case: AND of zero sub-steps is "all" (vacuous truth); OR
        // of zero sub-steps is "none". This mirrors SQL semantics.
        if (first) {
            if (step.combine_step->combine_op == PruneCombineOp::kAnd) {
                acc.reserve(static_cast<std::size_t>(context.nparts));
                for (int i = 0; i < context.nparts; ++i) {
                    acc.push_back(i);
                }
            }
            // OR of nothing: leave empty.
        }
        return Normalize(std::move(acc));
    }
    // Step with neither op_step nor combine_step: return all partitions
    // (treat as a no-op).
    std::vector<int> all;
    all.reserve(static_cast<std::size_t>(context.nparts));
    for (int i = 0; i < context.nparts; ++i) {
        all.push_back(i);
    }
    return all;
}

}  // namespace

// --- Step builders ---

std::unique_ptr<PartitionPruneStep> MakePruneStepOp(int step_id, PruneOp op, types::Datum value,
                                                    bool is_null) {
    auto step = std::make_unique<PartitionPruneStep>();
    step->step_id = step_id;
    step->op_step = std::make_unique<PruneStepOp>();
    step->op_step->op = op;
    step->op_step->value = value;
    step->op_step->is_null = is_null;
    return step;
}

std::unique_ptr<PartitionPruneStep> MakePruneStepCombine(
    int step_id, PruneCombineOp combine_op,
    std::vector<std::unique_ptr<PartitionPruneStep>> sub_steps) {
    auto step = std::make_unique<PartitionPruneStep>();
    step->step_id = step_id;
    step->combine_step = std::make_unique<PruneStepCombine>();
    step->combine_step->combine_op = combine_op;
    step->combine_step->sub_steps = std::move(sub_steps);
    return step;
}

// --- Public pruning entry points ---

std::vector<int> partprune_partitions(
    const PruningContext& context, const std::vector<std::unique_ptr<PartitionPruneStep>>& steps) {
    if (context.boundinfo == nullptr || context.nparts <= 0) {
        return {};
    }
    // With no steps, return all partitions (no pruning possible).
    if (steps.empty()) {
        std::vector<int> all;
        all.reserve(static_cast<std::size_t>(context.nparts));
        for (int i = 0; i < context.nparts; ++i) {
            all.push_back(i);
        }
        return all;
    }

    // AND together the top-level steps (mirrors PostgreSQL: top-level steps
    // are implicitly ANDed). If the caller wants OR semantics, they wrap
    // their steps in a single combine step.
    std::vector<int> acc;
    bool first = true;
    for (const auto& step : steps) {
        std::vector<int> sub = EvaluateStep(context, *step);
        if (first) {
            acc = std::move(sub);
            first = false;
        } else {
            acc = Intersect(acc, sub);
        }
    }
    if (first) {
        // No top-level steps after all (shouldn't happen given the empty
        // check above, but be defensive).
        acc.reserve(static_cast<std::size_t>(context.nparts));
        for (int i = 0; i < context.nparts; ++i) {
            acc.push_back(i);
        }
    }
    return Normalize(std::move(acc));
}

std::vector<int> partprune_from_opexprs(const PruningContext& context,
                                        const std::vector<PruneStepOp>& opexprs) {
    if (context.boundinfo == nullptr || context.nparts <= 0) {
        return {};
    }
    if (opexprs.empty()) {
        // No predicates: all partitions.
        std::vector<int> all;
        all.reserve(static_cast<std::size_t>(context.nparts));
        for (int i = 0; i < context.nparts; ++i) {
            all.push_back(i);
        }
        return all;
    }
    // AND together the opexprs.
    std::vector<int> acc;
    bool first = true;
    for (const auto& op : opexprs) {
        std::vector<int> sub = Normalize(EvaluateOpStep(context, op));
        if (first) {
            acc = std::move(sub);
            first = false;
        } else {
            acc = Intersect(acc, sub);
        }
    }
    if (first) {
        acc.reserve(static_cast<std::size_t>(context.nparts));
        for (int i = 0; i < context.nparts; ++i) {
            acc.push_back(i);
        }
    }
    return Normalize(std::move(acc));
}

}  // namespace pgcpp::partitioning

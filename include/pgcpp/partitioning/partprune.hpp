// partprune.hpp — partition pruning (M9 sub-task 15.20.2).
//
// Converts PostgreSQL's src/include/partitioning/partprune.h and the public
// API of src/backend/partitioning/partprune.c to C++20.
//
// PostgreSQL's partition pruning happens in two stages:
//   1. The planner (or executor) builds a list of PartitionPruneStep nodes
//      derived from restriction clauses on the partition key.
//   2. get_matching_partitions walks the steps, produces a Bitmapset of
//      surviving partition indexes, and returns it.
//
// We keep the same two-stage shape, replacing the Bitmapset with
// std::vector<int> for ownership safety. The supported step kinds are:
//   - PARTITIONPRUNE_OP:    a single operator-based predicate (eq/lt/gt ...)
//                           against a constant value.
//   - PARTITIONPRUNE_COMBINE: an AND/OR combination of sub-step results.

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "pgcpp/partitioning/partbounds.hpp"

namespace pgcpp::partitioning {

// PruneCombineOp — combinator for combine steps. Matches PostgreSQL's
// PartitionPruneCombineOp enum (which is private in C, exposed here for
// callers that build steps directly in tests).
enum class PruneCombineOp : int {
    kAnd = 0,
    kOr = 1,
};

// Comparison operators supported by PARTITIONPRUNE_OP steps. We use the
// strategy numbers consistent with btree access methods, mirroring
// PostgreSQL's BTEqualStrategyNumber et al.
enum class PruneOp : int {
    kLess = 1,          // BTLessStrategyNumber
    kLessEqual = 2,     // BTLessEqualStrategyNumber
    kEqual = 3,         // BTEqualStrategyNumber
    kGreaterEqual = 4,  // BTGreaterEqualStrategyNumber
    kGreater = 5,       // BTGreaterStrategyNumber
};

// Forward declaration of the base step.
struct PartitionPruneStep;

// PruneStepOp — operator-based pruning step. Represents a single predicate
// of the form "partition_key <op> value". The step is evaluated against the
// partition bound info to produce the surviving partition set.
struct PruneStepOp {
    PruneOp op = PruneOp::kEqual;
    types::Datum value = 0;
    bool is_null = false;
};

// PruneStepCombine — combinator step. ANDs or ORs the results of its
// sub-steps together.
struct PruneStepCombine {
    PruneCombineOp combine_op = PruneCombineOp::kAnd;
    std::vector<std::unique_ptr<PartitionPruneStep>> sub_steps;
};

// PartitionPruneStep — variant-like base for the two step kinds.
// `step_id` mirrors PostgreSQL's step_id used for indexing into the
// per-step comparison function arrays (kept here for API fidelity).
struct PartitionPruneStep {
    int step_id = 0;
    // Exactly one of the two below is non-null.
    std::unique_ptr<PruneStepOp> op_step;
    std::unique_ptr<PruneStepCombine> combine_step;
};

// Helper builders so callers don't need to assemble unique_ptrs by hand.
std::unique_ptr<PartitionPruneStep> MakePruneStepOp(int step_id, PruneOp op, types::Datum value,
                                                    bool is_null = false);
std::unique_ptr<PartitionPruneStep> MakePruneStepCombine(
    int step_id, PruneCombineOp combine_op,
    std::vector<std::unique_ptr<PartitionPruneStep>> sub_steps);

// PruningContext — runtime context for one pruning pass. Mirrors
// PostgreSQL's PartitionPruneContext (fields kept but trimmed to what the
// simplified API needs).
struct PruningContext {
    PartitionStrategy strategy = PartitionStrategy::kList;
    int partnatts = 1;
    int nparts = 0;
    const PartitionBoundInfoData* boundinfo = nullptr;

    // Scan opexprs as a list of (op, value, is_null) triples. This is a
    // convenience mirror of what the planner would populate; callers that
    // supply steps directly via partprune_partitions can leave this empty.
    std::vector<PruneStepOp> scan_opexprs;
};

// partprune_partitions — given a pruning context and a list of pruning
// steps, return the indexes of partitions that satisfy the predicate.
//
// Returns a sorted, de-duplicated vector of partition indexes in [0, nparts).
// Returns an empty vector if no partitions match. Combine steps are
// evaluated recursively: AND takes the intersection, OR takes the union.
std::vector<int> partprune_partitions(
    const PruningContext& context, const std::vector<std::unique_ptr<PartitionPruneStep>>& steps);

// partprune_from_opexprs — convenience: build steps automatically from a
// list of operator predicates, AND-ed together, then run pruning. Useful for
// the common case of "WHERE pk = 5".
std::vector<int> partprune_from_opexprs(const PruningContext& context,
                                        const std::vector<PruneStepOp>& opexprs);

}  // namespace pgcpp::partitioning

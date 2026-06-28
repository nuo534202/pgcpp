// partbounds.cpp — implementation of partition bound descriptors.
//
// Converts the public API of PostgreSQL's src/backend/partitioning/partbounds.c
// to C++20. The original C code stores partition bounds as a flat sorted
// Datum** array plus an integer index map, with a separate PartitionRangeDatumKind
// matrix for range partitions. Here we keep the same logical layout using
// std::vector for safe ownership and use std::sort for the sorting pass.
//
// Simplifications (called out in the project README):
//   - Datum comparison treats Datum as int64_t. A full type-aware comparison
//     would require the partition key's operator class and FmgrInfo lookup,
//     which depends on catalog/planner infrastructure not yet converted.
//   - MINVALUE/MAXVALUE range sentinels are encoded as INT64_MIN/INT64_MAX
//     in the datums array. This is lossless for the int-Datum case that
//     dominates tests and ClickBench.
//   - partition_hash_identity uses a simple integer hash (multiply by a
//     Knuth-style constant and xor-shift) rather than PostgreSQL's
//     compute_partition_hash_value, which mixes all key columns via their
//     type-specific hash support functions.

#include "mytoydb/partitioning/partbounds.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

#include "mytoydb/common/error/elog.hpp"

namespace mytoydb::partitioning {

namespace {

// Encode a PartitionRangeDatum as a Datum. MINVALUE/MAXVALUE are encoded as
// INT32_MIN/INT32_MAX sentinels so they compare correctly against int32-encoded
// Datums (Int32GetDatum produces a zero-extended uint32_t in the Datum). A
// concrete value is returned as-is.
types::Datum EncodeRangeDatum(const PartitionRangeDatum& d) {
    switch (d.kind) {
        case PartitionRangeDatumKind::kMinValue:
            return static_cast<types::Datum>(static_cast<uint32_t>(INT32_MIN));
        case PartitionRangeDatumKind::kMaxValue:
            return static_cast<types::Datum>(static_cast<uint32_t>(INT32_MAX));
        case PartitionRangeDatumKind::kValue:
        default:
            return d.value;
    }
}

// Total order on Datum using int32_t semantics. This matches Int32GetDatum's
// representation: a negative int32_t is stored as its zero-extended uint32_t
// pattern, so we truncate back to int32_t before comparing. MINVALUE/MAXVALUE
// sentinels (INT32_MIN/INT32_MAX) sort correctly under this interpretation.
int CompareDatum(types::Datum a, types::Datum b) {
    int32_t ia = static_cast<int32_t>(a);
    int32_t ib = static_cast<int32_t>(b);
    if (ia < ib)
        return -1;
    if (ia > ib)
        return 1;
    return 0;
}

}  // namespace

int partition_hash_identity(int modulus, types::Datum value) {
    if (modulus <= 0) {
        ereport(error::LogLevel::kError, "partition_hash_identity: modulus must be positive");
    }
    // Knuth-style integer hash: mix the bits, then take the remainder.
    // PostgreSQL mixes all key columns via compute_partition_hash_value;
    // for the simplified single-int-Datum API this is sufficient and
    // deterministic.
    uint64_t x = static_cast<uint64_t>(value);
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return static_cast<int>(x % static_cast<uint64_t>(modulus));
}

bool partition_bound_spec_accepts(const PartitionBoundSpec& spec, types::Datum value,
                                  bool is_null) {
    if (spec.is_default) {
        // Default partitions accept anything not covered elsewhere; callers
        // must check the boundinfo's accept logic for that. As a per-spec
        // helper we conservatively return false here (the default partition
        // does not "match" a specific value via its bound).
        return false;
    }
    if (is_null) {
        // NULLs are handled at the boundinfo level via null_index. A spec
        // does not declare NULL-acceptance in our simplified model.
        return false;
    }
    switch (spec.strategy) {
        case PartitionStrategy::kHash: {
            if (spec.modulus <= 0) {
                return false;
            }
            return partition_hash_identity(spec.modulus, value) == spec.remainder;
        }
        case PartitionStrategy::kList: {
            for (types::Datum v : spec.list_values) {
                if (CompareDatum(v, value) == 0) {
                    return true;
                }
            }
            return false;
        }
        case PartitionStrategy::kRange: {
            // Lower bound inclusive, upper bound exclusive. MINVALUE/MAXVALUE
            // sentinels behave as -inf/+inf.
            if (spec.lower_values.empty() || spec.upper_values.empty()) {
                return false;
            }
            types::Datum lower = EncodeRangeDatum(spec.lower_values.front());
            types::Datum upper = EncodeRangeDatum(spec.upper_values.front());
            return CompareDatum(lower, value) <= 0 && CompareDatum(value, upper) < 0;
        }
    }
    return false;
}

int partition_bound_accepts(const PartitionBoundInfoData& boundinfo, types::Datum value,
                            bool is_null) {
    // NULL handling.
    if (is_null) {
        if (boundinfo.null_index != -1) {
            return boundinfo.null_index;
        }
        return boundinfo.default_index;
    }

    switch (boundinfo.strategy) {
        case PartitionStrategy::kList: {
            // Linear scan for an exact match.
            for (std::size_t i = 0; i < boundinfo.datums.size(); ++i) {
                if (!boundinfo.datums[i].empty() &&
                    CompareDatum(boundinfo.datums[i][0], value) == 0) {
                    return boundinfo.indexes[i];
                }
            }
            // No exact match: route to default partition if any.
            return boundinfo.default_index;
        }
        case PartitionStrategy::kRange: {
            // Each datums[i] is {lower, upper}; find the partition whose
            // half-open range contains value.
            for (std::size_t i = 0; i < boundinfo.datums.size(); ++i) {
                const auto& bounds = boundinfo.datums[i];
                if (bounds.size() < 2) {
                    continue;
                }
                types::Datum lower = bounds[0];
                types::Datum upper = bounds[1];
                if (CompareDatum(lower, value) <= 0 && CompareDatum(value, upper) < 0) {
                    return boundinfo.indexes[i];
                }
            }
            return boundinfo.default_index;
        }
        case PartitionStrategy::kHash: {
            if (boundinfo.greatest_modulus <= 0) {
                return boundinfo.default_index;
            }
            int rem = partition_hash_identity(boundinfo.greatest_modulus, value);
            if (rem < 0 || rem >= static_cast<int>(boundinfo.indexes.size())) {
                return boundinfo.default_index;
            }
            int idx = boundinfo.indexes[rem];
            return idx != -1 ? idx : boundinfo.default_index;
        }
    }
    return boundinfo.default_index;
}

PartitionBoundInfoData partition_bounds_create(const std::vector<PartitionBoundSpec>& specs,
                                               PartitionStrategy strategy) {
    PartitionBoundInfoData bi;
    bi.strategy = strategy;

    if (strategy == PartitionStrategy::kList) {
        // Flatten: each list value becomes one datums entry pointing at its
        // partition. Then sort by value for deterministic lookup.
        for (std::size_t part_idx = 0; part_idx < specs.size(); ++part_idx) {
            const auto& spec = specs[part_idx];
            if (spec.is_default) {
                bi.default_index = static_cast<int>(part_idx);
                continue;
            }
            for (types::Datum v : spec.list_values) {
                bi.datums.push_back({v});
                bi.indexes.push_back(static_cast<int>(part_idx));
            }
        }
        // Pair-sort by datum value.
        std::vector<std::size_t> order(bi.datums.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            return CompareDatum(bi.datums[a][0], bi.datums[b][0]) < 0;
        });
        std::vector<std::vector<types::Datum>> new_datums(bi.datums.size());
        std::vector<int> new_indexes(bi.indexes.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            new_datums[i] = std::move(bi.datums[order[i]]);
            new_indexes[i] = bi.indexes[order[i]];
        }
        bi.datums = std::move(new_datums);
        bi.indexes = std::move(new_indexes);
        return bi;
    }

    if (strategy == PartitionStrategy::kRange) {
        // Each partition contributes a {lower, upper} datum pair. Then sort
        // partitions by lower bound so a linear scan finds the matching one.
        for (std::size_t part_idx = 0; part_idx < specs.size(); ++part_idx) {
            const auto& spec = specs[part_idx];
            if (spec.is_default) {
                bi.default_index = static_cast<int>(part_idx);
                continue;
            }
            if (spec.lower_values.empty() || spec.upper_values.empty()) {
                ereport(error::LogLevel::kError,
                        "partition_bounds_create: range spec missing bounds");
            }
            types::Datum lower = EncodeRangeDatum(spec.lower_values.front());
            types::Datum upper = EncodeRangeDatum(spec.upper_values.front());
            bi.datums.push_back({lower, upper});
            bi.indexes.push_back(static_cast<int>(part_idx));
        }
        // Sort by lower bound.
        std::vector<std::size_t> order(bi.datums.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            return CompareDatum(bi.datums[a][0], bi.datums[b][0]) < 0;
        });
        std::vector<std::vector<types::Datum>> new_datums(bi.datums.size());
        std::vector<int> new_indexes(bi.indexes.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            new_datums[i] = std::move(bi.datums[order[i]]);
            new_indexes[i] = bi.indexes[order[i]];
        }
        bi.datums = std::move(new_datums);
        bi.indexes = std::move(new_indexes);
        return bi;
    }

    if (strategy == PartitionStrategy::kHash) {
        // Compute greatest modulus, then for each remainder slot pick the
        // partition whose modulus divides it with the matching remainder.
        bi.greatest_modulus = 0;
        for (const auto& spec : specs) {
            if (spec.is_default) {
                continue;
            }
            if (spec.modulus > bi.greatest_modulus) {
                bi.greatest_modulus = spec.modulus;
            }
        }
        if (bi.greatest_modulus <= 0) {
            return bi;
        }
        bi.indexes.assign(static_cast<std::size_t>(bi.greatest_modulus), -1);
        for (std::size_t part_idx = 0; part_idx < specs.size(); ++part_idx) {
            const auto& spec = specs[part_idx];
            if (spec.is_default) {
                bi.default_index = static_cast<int>(part_idx);
                continue;
            }
            if (spec.modulus <= 0) {
                continue;
            }
            // For each remainder slot in [0, greatest_modulus), assign this
            // partition if (rem % spec.modulus) == spec.remainder.
            for (int rem = 0; rem < bi.greatest_modulus; ++rem) {
                if (rem % spec.modulus == spec.remainder) {
                    if (bi.indexes[rem] == -1) {
                        bi.indexes[rem] = static_cast<int>(part_idx);
                    }
                }
            }
        }
        // Also store datums as {modulus, remainder} pairs for inspection.
        for (const auto& spec : specs) {
            if (spec.is_default) {
                continue;
            }
            bi.datums.push_back({static_cast<types::Datum>(spec.modulus),
                                 static_cast<types::Datum>(spec.remainder)});
        }
        return bi;
    }

    ereport(error::LogLevel::kError, "partition_bounds_create: unknown partition strategy");
    // Unreachable — ereport(kError) longjmps. Returned to satisfy the
    // compiler's control-flow analysis.
    return bi;
}

}  // namespace mytoydb::partitioning

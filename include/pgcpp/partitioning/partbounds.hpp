// partbounds.hpp — partition bound descriptors (M9 sub-task 15.20.2).
//
// Converts PostgreSQL's src/include/partitioning/partbounds.h and the public
// data structures of src/backend/partitioning/partbounds.c to C++20.
//
// PostgreSQL stores partition bounds as a sorted Datum array plus an integer
// index map. This faithful but compact re-implementation keeps the same
// public API surface (strategy, datums, indexes, null/default slots) while
// using std::vector for ownership. The simplification is intentional: a
// later phase can swap the in-memory representation for a page-resident one
// once the storage manager is wired up.

#pragma once

#include <cstdint>
#include <vector>

#include "pgcpp/types/datum.hpp"

namespace mytoydb::partitioning {

// Oid — PostgreSQL's object identifier. We define a local alias to avoid a
// hard dependency on catalog.hpp; it is the same uint32_t as catalog::Oid.
using Oid = uint32_t;

// Partition strategy codes (PostgreSQL PARTITION_STRATEGY_* macros).
// Kept as an enum class with the original char values for compatibility.
enum class PartitionStrategy : char {
    kHash = 'h',
    kList = 'l',
    kRange = 'r',
};

// PartitionRangeDatumKind — semantic kind of a single range bound datum.
// Matches PostgreSQL's PARTITION_RANGE_DATUM_* enum values.
enum class PartitionRangeDatumKind : int {
    kMinValue = -1,
    kValue = 0,
    kMaxValue = 1,
};

// A single datum of a range bound. A range partition has a list of these for
// the lower bound and a list for the upper bound (one per partition key
// column).
struct PartitionRangeDatum {
    PartitionRangeDatumKind kind = PartitionRangeDatumKind::kValue;
    types::Datum value = 0;
};

// PartitionBoundSpec — a single partition's bound specification, matching
// PostgreSQL's parsenodes.h PartitionBoundSpec. The active fields depend on
// `strategy`:
//   - HASH:    modulus + remainder
//   - LIST:    list_values
//   - RANGE:   lower_values + upper_values
// `is_default` (when true) marks the catch-all DEFAULT partition; in that
// case the strategy-specific fields are unused.
struct PartitionBoundSpec {
    PartitionStrategy strategy = PartitionStrategy::kList;
    bool is_default = false;

    // HASH
    int modulus = 0;
    int remainder = 0;

    // LIST — one entry per accepted value.
    std::vector<types::Datum> list_values;

    // RANGE — one entry per partition key column for lower/upper bound.
    std::vector<PartitionRangeDatum> lower_values;
    std::vector<PartitionRangeDatum> upper_values;
};

// PartitionBoundInfoData — the merged, sorted view over all partition bounds
// for a partitioned table. This is what partition pruning consults.
//
// In PostgreSQL this struct stores datums as Datum** with a separate `kind`
// matrix for range bounds. Here we keep the same logical layout using
// std::vector for safe ownership. `datums[i]` is the i-th sorted bound;
// `indexes[i]` is the partition index that bound maps to.
//
//   - LIST:  nindexes == ndatums.
//   - RANGE: nindexes == ndatums + 1 (extra entry for values above the last
//            bound; -1 for gaps).
//   - HASH:  nindexes == greatest modulus; indexes[rem] is the partition
//            accepting that remainder (or -1).
struct PartitionBoundInfoData {
    PartitionStrategy strategy = PartitionStrategy::kList;

    // Sorted datums. Each inner vector has length partnatts (1 in the
    // simplified single-column case). For RANGE, an extra `kind` matrix is
    // not stored separately — min/max kinds are encoded inline by the bound
    // creator and respected by partition_bound_accepts.
    std::vector<std::vector<types::Datum>> datums;

    // Partition index for each datum entry (or -1 for gaps in RANGE).
    std::vector<int> indexes;

    // Hash-specific: the greatest modulus across all partitions.
    int greatest_modulus = 0;

    // Index of the partition accepting NULLs (-1 if none).
    int null_index = -1;

    // Index of the default partition (-1 if none).
    int default_index = -1;
};

// Opaque handle preserved for API compatibility with PostgreSQL.
using PartitionBoundInfo = PartitionBoundInfoData*;

// partition_bound_accepts_nulls — true if the bound info has a NULL-accepting
// partition. PostgreSQL implements this as a macro.
inline bool partition_bound_accepts_nulls(const PartitionBoundInfoData& bi) {
    return bi.null_index != -1;
}

// partition_bound_has_default — true if the bound info has a default
// partition. PostgreSQL implements this as a macro.
inline bool partition_bound_has_default(const PartitionBoundInfoData& bi) {
    return bi.default_index != -1;
}

// partition_bound_accepts — return the partition index that accepts the
// given value, or -1 if no partition matches (the caller should then route
// to the default partition, if any). Returns the null_index for a null value
// when one is configured.
//
// For LIST: linear scan of datums for an exact Datum equality match.
// For RANGE: linear scan for the partition whose [lower, upper) range
//           contains the value, honouring MINVALUE/MAXVALUE sentinels.
// For HASH:  value % greatest_modulus indexes into the partition table.
int partition_bound_accepts(const PartitionBoundInfoData& boundinfo, types::Datum value,
                            bool is_null = false);

// partition_hash_identity — compute the (modulus, remainder) identity for
// the given value under a hash partitioned table. PostgreSQL uses
// compute_partition_hash_value(); here we reduce to a simple integer hash
// since the simplified API only deals with integer Datums. Returns the
// remainder modulo `modulus`.
int partition_hash_identity(int modulus, types::Datum value);

// partition_bounds_create — build a PartitionBoundInfoData from a list of
// PartitionBoundSpec. The specs are sorted and the index map is filled.
// `nparts` is the number of partitions (must equal specs.size()).
PartitionBoundInfoData partition_bounds_create(const std::vector<PartitionBoundSpec>& specs,
                                               PartitionStrategy strategy);

// partition_bound_spec_accepts — check whether a single PartitionBoundSpec
// accepts the given value. Used both for bound construction validation and
// for tests. Returns true if the value falls inside this partition's bound.
bool partition_bound_spec_accepts(const PartitionBoundSpec& spec, types::Datum value,
                                  bool is_null = false);

}  // namespace mytoydb::partitioning

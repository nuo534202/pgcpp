// partitioning_test.cpp — unit tests for the M9 partitioning module
// (SubTask 15.20.2).
//
// Covers:
//   - List partition bounds: create, accept value, reject value.
//   - Range partition bounds: create, accept value in range, reject
//     out-of-range.
//   - Hash partition bounds: modulus/remainder.
//   - Partition descriptor cache: lookup hit/miss, create.
//   - Partition pruning: prune by list membership, prune by range bounds,
//     combine AND/OR steps.

#include "pgcpp/partitioning/partitioning.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/alloc_set.hpp"
#include "pgcpp/common/memory/memory_context.hpp"
#include "pgcpp/types/datum.hpp"

namespace {

using mytoydb::error::ErrorData;
using mytoydb::error::LogLevel;
using mytoydb::memory::AllocSetContext;
using mytoydb::partitioning::MakePruneStepCombine;
using mytoydb::partitioning::MakePruneStepOp;
using mytoydb::partitioning::Oid;
using mytoydb::partitioning::PartitionBoundInfoData;
using mytoydb::partitioning::PartitionBoundSpec;
using mytoydb::partitioning::PartitionDescriptorCache;
using mytoydb::partitioning::PartitionRangeDatum;
using mytoydb::partitioning::PartitionRangeDatumKind;
using mytoydb::partitioning::PartitionStrategy;
using mytoydb::partitioning::PruneCombineOp;
using mytoydb::partitioning::PruneOp;
using mytoydb::partitioning::PruneStepCombine;
using mytoydb::partitioning::PruneStepOp;
using mytoydb::partitioning::PruningContext;
using mytoydb::types::Datum;
using mytoydb::types::Int32GetDatum;

// Convenience: build a list-value PartitionBoundSpec.
PartitionBoundSpec MakeListSpec(std::vector<int32_t> values) {
    PartitionBoundSpec spec;
    spec.strategy = PartitionStrategy::kList;
    for (int32_t v : values) {
        spec.list_values.push_back(Int32GetDatum(v));
    }
    return spec;
}

// Convenience: build a range PartitionBoundSpec with concrete bounds.
PartitionBoundSpec MakeRangeSpec(int32_t lower, int32_t upper) {
    PartitionBoundSpec spec;
    spec.strategy = PartitionStrategy::kRange;
    PartitionRangeDatum lo;
    lo.kind = PartitionRangeDatumKind::kValue;
    lo.value = Int32GetDatum(lower);
    PartitionRangeDatum up;
    up.kind = PartitionRangeDatumKind::kValue;
    up.value = Int32GetDatum(upper);
    spec.lower_values.push_back(lo);
    spec.upper_values.push_back(up);
    return spec;
}

// Convenience: build a hash PartitionBoundSpec.
PartitionBoundSpec MakeHashSpec(int modulus, int remainder) {
    PartitionBoundSpec spec;
    spec.strategy = PartitionStrategy::kHash;
    spec.modulus = modulus;
    spec.remainder = remainder;
    return spec;
}

// Common fixture: initializes the error subsystem and a memory context that
// backs all palloc allocations made under test.
class PartitioningTest : public ::testing::Test {
protected:
    void SetUp() override {
        mytoydb::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("partitioning_test_context");
        mytoydb::memory::SetCurrentMemoryContext(context_);
    }

    void TearDown() override {
        mytoydb::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    AllocSetContext* context_ = nullptr;
};

// =============================================================================
// List partition bounds
// =============================================================================

TEST_F(PartitioningTest, ListBoundAcceptsValueInList) {
    auto spec = MakeListSpec({10, 20, 30});
    EXPECT_TRUE(mytoydb::partitioning::partition_bound_spec_accepts(spec, Int32GetDatum(10)));
    EXPECT_TRUE(mytoydb::partitioning::partition_bound_spec_accepts(spec, Int32GetDatum(20)));
    EXPECT_TRUE(mytoydb::partitioning::partition_bound_spec_accepts(spec, Int32GetDatum(30)));
}

TEST_F(PartitioningTest, ListBoundRejectsValueNotInList) {
    auto spec = MakeListSpec({10, 20, 30});
    EXPECT_FALSE(mytoydb::partitioning::partition_bound_spec_accepts(spec, Int32GetDatum(15)));
    EXPECT_FALSE(mytoydb::partitioning::partition_bound_spec_accepts(spec, Int32GetDatum(99)));
    EXPECT_FALSE(mytoydb::partitioning::partition_bound_spec_accepts(spec, Int32GetDatum(0)));
}

TEST_F(PartitioningTest, ListBoundInfoAcceptsRoutesToCorrectPartition) {
    // Partition 0: {10, 20}; Partition 1: {30, 40}.
    std::vector<PartitionBoundSpec> specs = {MakeListSpec({10, 20}), MakeListSpec({30, 40})};
    auto bi = mytoydb::partitioning::partition_bounds_create(specs, PartitionStrategy::kList);
    EXPECT_EQ(mytoydb::partitioning::partition_bound_accepts(bi, Int32GetDatum(10)), 0);
    EXPECT_EQ(mytoydb::partitioning::partition_bound_accepts(bi, Int32GetDatum(20)), 0);
    EXPECT_EQ(mytoydb::partitioning::partition_bound_accepts(bi, Int32GetDatum(30)), 1);
    EXPECT_EQ(mytoydb::partitioning::partition_bound_accepts(bi, Int32GetDatum(40)), 1);
    // No match -> -1 (no default partition configured).
    EXPECT_EQ(mytoydb::partitioning::partition_bound_accepts(bi, Int32GetDatum(15)), -1);
}

// =============================================================================
// Range partition bounds
// =============================================================================

TEST_F(PartitioningTest, RangeBoundAcceptsValueInRange) {
    auto spec = MakeRangeSpec(10, 20);
    // Half-open: [10, 20).
    EXPECT_TRUE(mytoydb::partitioning::partition_bound_spec_accepts(spec, Int32GetDatum(10)));
    EXPECT_TRUE(mytoydb::partitioning::partition_bound_spec_accepts(spec, Int32GetDatum(15)));
    EXPECT_TRUE(mytoydb::partitioning::partition_bound_spec_accepts(spec, Int32GetDatum(19)));
}

TEST_F(PartitioningTest, RangeBoundRejectsOutOfRange) {
    auto spec = MakeRangeSpec(10, 20);
    EXPECT_FALSE(mytoydb::partitioning::partition_bound_spec_accepts(spec, Int32GetDatum(9)));
    EXPECT_FALSE(mytoydb::partitioning::partition_bound_spec_accepts(
        spec, Int32GetDatum(20)));  // upper bound exclusive
    EXPECT_FALSE(mytoydb::partitioning::partition_bound_spec_accepts(spec, Int32GetDatum(21)));
}

TEST_F(PartitioningTest, RangeBoundInfoAcceptsRoutesToCorrectPartition) {
    // Three contiguous range partitions: [10,20), [20,40), [40,60).
    std::vector<PartitionBoundSpec> specs = {MakeRangeSpec(10, 20), MakeRangeSpec(20, 40),
                                             MakeRangeSpec(40, 60)};
    auto bi = mytoydb::partitioning::partition_bounds_create(specs, PartitionStrategy::kRange);
    EXPECT_EQ(mytoydb::partitioning::partition_bound_accepts(bi, Int32GetDatum(15)), 0);
    EXPECT_EQ(mytoydb::partitioning::partition_bound_accepts(bi, Int32GetDatum(10)), 0);
    EXPECT_EQ(mytoydb::partitioning::partition_bound_accepts(bi, Int32GetDatum(25)), 1);
    EXPECT_EQ(mytoydb::partitioning::partition_bound_accepts(bi, Int32GetDatum(39)), 1);
    EXPECT_EQ(mytoydb::partitioning::partition_bound_accepts(bi, Int32GetDatum(50)), 2);
    // Above all bounds -> -1 (no default).
    EXPECT_EQ(mytoydb::partitioning::partition_bound_accepts(bi, Int32GetDatum(60)), -1);
    // Below all bounds -> -1.
    EXPECT_EQ(mytoydb::partitioning::partition_bound_accepts(bi, Int32GetDatum(5)), -1);
}

TEST_F(PartitioningTest, RangeBoundWithMinMaxValueSentinels) {
    // First partition: [MINVALUE, 10); Last partition: [50, MAXVALUE).
    PartitionBoundSpec first;
    first.strategy = PartitionStrategy::kRange;
    PartitionRangeDatum lo_min;
    lo_min.kind = PartitionRangeDatumKind::kMinValue;
    first.lower_values.push_back(lo_min);
    PartitionRangeDatum up_10;
    up_10.kind = PartitionRangeDatumKind::kValue;
    up_10.value = Int32GetDatum(10);
    first.upper_values.push_back(up_10);

    PartitionBoundSpec last;
    last.strategy = PartitionStrategy::kRange;
    PartitionRangeDatum lo_50;
    lo_50.kind = PartitionRangeDatumKind::kValue;
    lo_50.value = Int32GetDatum(50);
    last.lower_values.push_back(lo_50);
    PartitionRangeDatum up_max;
    up_max.kind = PartitionRangeDatumKind::kMaxValue;
    last.upper_values.push_back(up_max);

    std::vector<PartitionBoundSpec> specs = {first, last};
    auto bi = mytoydb::partitioning::partition_bounds_create(specs, PartitionStrategy::kRange);
    EXPECT_EQ(mytoydb::partitioning::partition_bound_accepts(bi, Int32GetDatum(-1000)), 0);
    EXPECT_EQ(mytoydb::partitioning::partition_bound_accepts(bi, Int32GetDatum(9)), 0);
    EXPECT_EQ(mytoydb::partitioning::partition_bound_accepts(bi, Int32GetDatum(50)), 1);
    EXPECT_EQ(mytoydb::partitioning::partition_bound_accepts(bi, Int32GetDatum(10000)), 1);
    // Gap (10, 50): no partition matches.
    EXPECT_EQ(mytoydb::partitioning::partition_bound_accepts(bi, Int32GetDatum(30)), -1);
}

// =============================================================================
// Hash partition bounds
// =============================================================================

TEST_F(PartitioningTest, HashBoundAcceptsValueWithMatchingRemainder) {
    auto spec = MakeHashSpec(/*modulus=*/4, /*remainder=*/1);
    // Verify the spec accepts values whose hash remainder (mod 4) is 1.
    for (int32_t v = 0; v < 100; ++v) {
        int rem = mytoydb::partitioning::partition_hash_identity(4, Int32GetDatum(v));
        bool expected = (rem == 1);
        bool actual = mytoydb::partitioning::partition_bound_spec_accepts(spec, Int32GetDatum(v));
        EXPECT_EQ(expected, actual) << "value=" << v;
    }
}

TEST_F(PartitioningTest, HashBoundInfoRoutesToCorrectPartition) {
    // Two partitions, modulus 4: partition 0 -> rem 0,1; partition 1 -> rem 2,3.
    // (PostgreSQL splits a 4-modulus table into two 2-modulus partitions
    // with remainders 0 and 1.)
    std::vector<PartitionBoundSpec> specs = {MakeHashSpec(2, 0), MakeHashSpec(2, 1)};
    auto bi = mytoydb::partitioning::partition_bounds_create(specs, PartitionStrategy::kHash);
    EXPECT_EQ(bi.greatest_modulus, 2);
    // Each remainder slot should map to a partition.
    ASSERT_EQ(static_cast<int>(bi.indexes.size()), 2);
    EXPECT_EQ(bi.indexes[0], 0);
    EXPECT_EQ(bi.indexes[1], 1);

    // For each int value, partition_bound_accepts should agree with the
    // spec_accepts of the matched partition.
    for (int32_t v = 0; v < 50; ++v) {
        int idx = mytoydb::partitioning::partition_bound_accepts(bi, Int32GetDatum(v));
        ASSERT_TRUE(idx == 0 || idx == 1) << "value=" << v;
        EXPECT_TRUE(mytoydb::partitioning::partition_bound_spec_accepts(
            specs[static_cast<std::size_t>(idx)], Int32GetDatum(v)))
            << "value=" << v << " idx=" << idx;
    }
}

// =============================================================================
// Partition descriptor cache
// =============================================================================

TEST_F(PartitioningTest, PartitionDescriptorCacheCreateAndLookupHit) {
    PartitionDescriptorCache cache;
    std::vector<PartitionBoundSpec> specs = {MakeRangeSpec(10, 20), MakeRangeSpec(20, 40)};
    std::vector<Oid> oids = {1001, 1002};
    std::vector<bool> is_leaf = {true, true};

    auto* desc = cache.Create(/*parent_oid=*/42, specs, oids, is_leaf, PartitionStrategy::kRange);
    ASSERT_NE(desc, nullptr);
    EXPECT_EQ(desc->nparts, 2);
    EXPECT_EQ(desc->oids.size(), 2u);
    EXPECT_EQ(desc->oids[0], 1001u);
    EXPECT_EQ(desc->oids[1], 1002u);
    EXPECT_EQ(desc->boundinfo.strategy, PartitionStrategy::kRange);

    auto* lookup = cache.Lookup(/*parent_oid=*/42);
    EXPECT_EQ(lookup, desc);
    EXPECT_EQ(cache.Size(), 1u);
}

TEST_F(PartitioningTest, PartitionDescriptorCacheLookupMissReturnsNull) {
    PartitionDescriptorCache cache;
    EXPECT_EQ(cache.Lookup(/*parent_oid=*/999), nullptr);
    EXPECT_EQ(cache.Size(), 0u);
}

TEST_F(PartitioningTest, PartitionDescriptorCacheLookupPartitionByOid) {
    PartitionDescriptorCache cache;
    std::vector<PartitionBoundSpec> specs = {MakeListSpec({10, 20}), MakeListSpec({30, 40})};
    std::vector<Oid> oids = {1001, 1002};
    std::vector<bool> is_leaf = {true, true};

    cache.Create(/*parent_oid=*/42, specs, oids, is_leaf, PartitionStrategy::kList);
    auto* parent = cache.LookupPartitionByOid(/*partition_oid=*/1002);
    ASSERT_NE(parent, nullptr);
    EXPECT_EQ(parent->nparts, 2);
    // Miss.
    EXPECT_EQ(cache.LookupPartitionByOid(/*partition_oid=*/9999), nullptr);
}

TEST_F(PartitioningTest, PartitionDescriptorCacheInvalidateAndReplace) {
    PartitionDescriptorCache cache;
    std::vector<PartitionBoundSpec> specs = {MakeRangeSpec(0, 10)};
    std::vector<Oid> oids = {1};
    std::vector<bool> is_leaf = {true};

    cache.Create(/*parent_oid=*/7, specs, oids, is_leaf, PartitionStrategy::kRange);
    ASSERT_NE(cache.Lookup(7), nullptr);
    EXPECT_TRUE(cache.Invalidate(7));
    EXPECT_EQ(cache.Lookup(7), nullptr);
    EXPECT_FALSE(cache.Invalidate(7));

    // Replacing an existing entry should swap the descriptor.
    cache.Create(/*parent_oid=*/7, {MakeRangeSpec(0, 100)}, {1}, {true}, PartitionStrategy::kRange);
    auto* replaced = cache.Lookup(7);
    ASSERT_NE(replaced, nullptr);
    EXPECT_EQ(replaced->boundinfo.datums.size(), 1u);
}

TEST_F(PartitioningTest, GetDefaultOidFromPartDescWithNoDefault) {
    PartitionDescriptorCache cache;
    std::vector<PartitionBoundSpec> specs = {MakeListSpec({10})};
    std::vector<Oid> oids = {1234};
    std::vector<bool> is_leaf = {true};
    auto* desc = cache.Create(/*parent_oid=*/1, specs, oids, is_leaf, PartitionStrategy::kList);
    EXPECT_EQ(mytoydb::partitioning::get_default_oid_from_partdesc(desc), 0u);
    EXPECT_EQ(mytoydb::partitioning::get_default_oid_from_partdesc(nullptr), 0u);
}

TEST_F(PartitioningTest, GetDefaultOidFromPartDescWithDefault) {
    PartitionDescriptorCache cache;
    PartitionBoundSpec def;
    def.strategy = PartitionStrategy::kList;
    def.is_default = true;
    std::vector<PartitionBoundSpec> specs = {MakeListSpec({10, 20}), def};
    std::vector<Oid> oids = {1234, 5678};
    std::vector<bool> is_leaf = {true, true};
    auto* desc = cache.Create(/*parent_oid=*/1, specs, oids, is_leaf, PartitionStrategy::kList);
    EXPECT_EQ(mytoydb::partitioning::get_default_oid_from_partdesc(desc), 5678u);
}

// =============================================================================
// Partition pruning — list membership (range example: value=15)
// =============================================================================

TEST_F(PartitioningTest, PruneByRangeEqualValue15) {
    // Three range partitions: [10,20), [30,40), [50,60).
    // WHERE pk = 15 should prune to partition 0.
    std::vector<PartitionBoundSpec> specs = {MakeRangeSpec(10, 20), MakeRangeSpec(30, 40),
                                             MakeRangeSpec(50, 60)};
    auto bi = mytoydb::partitioning::partition_bounds_create(specs, PartitionStrategy::kRange);

    PruningContext ctx;
    ctx.strategy = PartitionStrategy::kRange;
    ctx.partnatts = 1;
    ctx.nparts = 3;
    ctx.boundinfo = &bi;

    std::vector<std::unique_ptr<mytoydb::partitioning::PartitionPruneStep>> steps;
    steps.push_back(MakePruneStepOp(/*step_id=*/0, PruneOp::kEqual, Int32GetDatum(15)));

    auto matches = mytoydb::partitioning::partprune_partitions(ctx, steps);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0], 0);
}

TEST_F(PartitioningTest, PruneByListEqualValueMatchesOnePartition) {
    // Two list partitions: {10,20}, {30,40}. WHERE pk = 30 -> partition 1.
    std::vector<PartitionBoundSpec> specs = {MakeListSpec({10, 20}), MakeListSpec({30, 40})};
    auto bi = mytoydb::partitioning::partition_bounds_create(specs, PartitionStrategy::kList);

    PruningContext ctx;
    ctx.strategy = PartitionStrategy::kList;
    ctx.partnatts = 1;
    ctx.nparts = 2;
    ctx.boundinfo = &bi;

    std::vector<std::unique_ptr<mytoydb::partitioning::PartitionPruneStep>> steps;
    steps.push_back(MakePruneStepOp(/*step_id=*/0, PruneOp::kEqual, Int32GetDatum(30)));

    auto matches = mytoydb::partitioning::partprune_partitions(ctx, steps);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0], 1);
}

TEST_F(PartitioningTest, PruneByRangeLessThan) {
    // Range partitions: [10,20), [20,40), [40,60).
    // WHERE pk < 25 should match partitions 0 and 1 (their range extends
    // below 25).
    std::vector<PartitionBoundSpec> specs = {MakeRangeSpec(10, 20), MakeRangeSpec(20, 40),
                                             MakeRangeSpec(40, 60)};
    auto bi = mytoydb::partitioning::partition_bounds_create(specs, PartitionStrategy::kRange);

    PruningContext ctx;
    ctx.strategy = PartitionStrategy::kRange;
    ctx.partnatts = 1;
    ctx.nparts = 3;
    ctx.boundinfo = &bi;

    std::vector<std::unique_ptr<mytoydb::partitioning::PartitionPruneStep>> steps;
    steps.push_back(MakePruneStepOp(/*step_id=*/0, PruneOp::kLess, Int32GetDatum(25)));

    auto matches = mytoydb::partitioning::partprune_partitions(ctx, steps);
    // Partitions [10,20) and [20,40) both have lower < 25.
    ASSERT_EQ(matches.size(), 2u);
    EXPECT_EQ(matches[0], 0);
    EXPECT_EQ(matches[1], 1);
}

TEST_F(PartitioningTest, PruneByRangeGreaterThan) {
    // Range partitions: [10,20), [20,40), [40,60).
    // WHERE pk > 25 should match partitions 1 and 2 (their range extends
    // above 25).
    std::vector<PartitionBoundSpec> specs = {MakeRangeSpec(10, 20), MakeRangeSpec(20, 40),
                                             MakeRangeSpec(40, 60)};
    auto bi = mytoydb::partitioning::partition_bounds_create(specs, PartitionStrategy::kRange);

    PruningContext ctx;
    ctx.strategy = PartitionStrategy::kRange;
    ctx.partnatts = 1;
    ctx.nparts = 3;
    ctx.boundinfo = &bi;

    std::vector<std::unique_ptr<mytoydb::partitioning::PartitionPruneStep>> steps;
    steps.push_back(MakePruneStepOp(/*step_id=*/0, PruneOp::kGreater, Int32GetDatum(25)));

    auto matches = mytoydb::partitioning::partprune_partitions(ctx, steps);
    // Partitions [20,40) (upper 40 > 25+1) and [40,60) (upper 60 > 25+1).
    ASSERT_EQ(matches.size(), 2u);
    EXPECT_EQ(matches[0], 1);
    EXPECT_EQ(matches[1], 2);
}

TEST_F(PartitioningTest, PruneByListLessThanMatchesPartitionsWithLowValue) {
    // List partitions: {5, 100}, {50, 60}. WHERE pk < 10 -> partition 0
    // (has value 5 < 10) only.
    std::vector<PartitionBoundSpec> specs = {MakeListSpec({5, 100}), MakeListSpec({50, 60})};
    auto bi = mytoydb::partitioning::partition_bounds_create(specs, PartitionStrategy::kList);

    PruningContext ctx;
    ctx.strategy = PartitionStrategy::kList;
    ctx.partnatts = 1;
    ctx.nparts = 2;
    ctx.boundinfo = &bi;

    std::vector<std::unique_ptr<mytoydb::partitioning::PartitionPruneStep>> steps;
    steps.push_back(MakePruneStepOp(/*step_id=*/0, PruneOp::kLess, Int32GetDatum(10)));

    auto matches = mytoydb::partitioning::partprune_partitions(ctx, steps);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0], 0);
}

// =============================================================================
// Combine steps (AND/OR)
// =============================================================================

TEST_F(PartitioningTest, PruneCombineAndIntersects) {
    // Range partitions: [10,20), [20,40), [40,60).
    // WHERE pk >= 15 AND pk < 25 should match partitions 0 and 1 (their
    // intersection: partition 0 has values >= 15 in [15,20), partition 1
    // has values < 25 in [20,25)).
    std::vector<PartitionBoundSpec> specs = {MakeRangeSpec(10, 20), MakeRangeSpec(20, 40),
                                             MakeRangeSpec(40, 60)};
    auto bi = mytoydb::partitioning::partition_bounds_create(specs, PartitionStrategy::kRange);

    PruningContext ctx;
    ctx.strategy = PartitionStrategy::kRange;
    ctx.partnatts = 1;
    ctx.nparts = 3;
    ctx.boundinfo = &bi;

    std::vector<std::unique_ptr<mytoydb::partitioning::PartitionPruneStep>> sub_steps;
    sub_steps.push_back(MakePruneStepOp(/*step_id=*/1, PruneOp::kGreaterEqual, Int32GetDatum(15)));
    sub_steps.push_back(MakePruneStepOp(/*step_id=*/2, PruneOp::kLess, Int32GetDatum(25)));

    std::vector<std::unique_ptr<mytoydb::partitioning::PartitionPruneStep>> steps;
    steps.push_back(
        MakePruneStepCombine(/*step_id=*/0, PruneCombineOp::kAnd, std::move(sub_steps)));

    auto matches = mytoydb::partitioning::partprune_partitions(ctx, steps);
    // Partition 0 ([10,20)): lower 10 <= 15 satisfies GE, lower 10 < 25
    //   satisfies LT (since lower < 25). Match.
    // Partition 1 ([20,40)): upper 40 > 15+1 satisfies GE, upper 40 > 25+1
    //   satisfies GT. Match.
    // Partition 2 ([40,60)): upper 60 > 15+1 satisfies GE, upper 60 > 25+1
    //   satisfies GT. Also matches LT-GT! Wait...
    //
    // Re-check: kLess (key < 25) for [40,60) means a value v with v < 25 in
    // [40,60). There is no such v because lower=40 > 25. So partition 2 does
    // NOT match kLess. So AND result should be {0, 1}.
    ASSERT_EQ(matches.size(), 2u);
    EXPECT_EQ(matches[0], 0);
    EXPECT_EQ(matches[1], 1);
}

TEST_F(PartitioningTest, PruneCombineOrUnions) {
    // Range partitions: [10,20), [20,40), [40,60).
    // WHERE pk = 5 OR pk = 50: pk=5 matches no partition (below all bounds),
    // pk=50 matches partition 2. OR should yield {2}.
    std::vector<PartitionBoundSpec> specs = {MakeRangeSpec(10, 20), MakeRangeSpec(20, 40),
                                             MakeRangeSpec(40, 60)};
    auto bi = mytoydb::partitioning::partition_bounds_create(specs, PartitionStrategy::kRange);

    PruningContext ctx;
    ctx.strategy = PartitionStrategy::kRange;
    ctx.partnatts = 1;
    ctx.nparts = 3;
    ctx.boundinfo = &bi;

    std::vector<std::unique_ptr<mytoydb::partitioning::PartitionPruneStep>> sub_steps;
    sub_steps.push_back(MakePruneStepOp(/*step_id=*/1, PruneOp::kEqual, Int32GetDatum(5)));
    sub_steps.push_back(MakePruneStepOp(/*step_id=*/2, PruneOp::kEqual, Int32GetDatum(50)));

    std::vector<std::unique_ptr<mytoydb::partitioning::PartitionPruneStep>> steps;
    steps.push_back(MakePruneStepCombine(/*step_id=*/0, PruneCombineOp::kOr, std::move(sub_steps)));

    auto matches = mytoydb::partitioning::partprune_partitions(ctx, steps);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0], 2);
}

TEST_F(PartitioningTest, PruneCombineAndOfTwoEqualsYieldsSinglePartition) {
    // WHERE pk = 15 AND pk = 25 -> no partition matches both (15 in
    // partition 0, 25 in partition 1, intersection is empty).
    std::vector<PartitionBoundSpec> specs = {MakeRangeSpec(10, 20), MakeRangeSpec(20, 40)};
    auto bi = mytoydb::partitioning::partition_bounds_create(specs, PartitionStrategy::kRange);

    PruningContext ctx;
    ctx.strategy = PartitionStrategy::kRange;
    ctx.partnatts = 1;
    ctx.nparts = 2;
    ctx.boundinfo = &bi;

    std::vector<std::unique_ptr<mytoydb::partitioning::PartitionPruneStep>> sub_steps;
    sub_steps.push_back(MakePruneStepOp(/*step_id=*/1, PruneOp::kEqual, Int32GetDatum(15)));
    sub_steps.push_back(MakePruneStepOp(/*step_id=*/2, PruneOp::kEqual, Int32GetDatum(25)));

    std::vector<std::unique_ptr<mytoydb::partitioning::PartitionPruneStep>> steps;
    steps.push_back(
        MakePruneStepCombine(/*step_id=*/0, PruneCombineOp::kAnd, std::move(sub_steps)));

    auto matches = mytoydb::partitioning::partprune_partitions(ctx, steps);
    EXPECT_TRUE(matches.empty());
}

// =============================================================================
// Top-level multi-step AND (implicit)
// =============================================================================

TEST_F(PartitioningTest, PruneTopLevelStepsAreAnded) {
    // Top-level steps are ANDed: WHERE pk = 50 (one top-level step) on a
    // table where 50 is in partition 2 should yield {2}.
    std::vector<PartitionBoundSpec> specs = {MakeRangeSpec(10, 20), MakeRangeSpec(20, 40),
                                             MakeRangeSpec(40, 60)};
    auto bi = mytoydb::partitioning::partition_bounds_create(specs, PartitionStrategy::kRange);

    PruningContext ctx;
    ctx.strategy = PartitionStrategy::kRange;
    ctx.partnatts = 1;
    ctx.nparts = 3;
    ctx.boundinfo = &bi;

    std::vector<std::unique_ptr<mytoydb::partitioning::PartitionPruneStep>> steps;
    steps.push_back(MakePruneStepOp(/*step_id=*/0, PruneOp::kEqual, Int32GetDatum(50)));

    auto matches = mytoydb::partitioning::partprune_partitions(ctx, steps);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0], 2);
}

TEST_F(PartitioningTest, PruneEmptyStepsReturnsAll) {
    std::vector<PartitionBoundSpec> specs = {MakeRangeSpec(10, 20), MakeRangeSpec(20, 40)};
    auto bi = mytoydb::partitioning::partition_bounds_create(specs, PartitionStrategy::kRange);

    PruningContext ctx;
    ctx.strategy = PartitionStrategy::kRange;
    ctx.partnatts = 1;
    ctx.nparts = 2;
    ctx.boundinfo = &bi;

    std::vector<std::unique_ptr<mytoydb::partitioning::PartitionPruneStep>> steps;
    auto matches = mytoydb::partitioning::partprune_partitions(ctx, steps);
    ASSERT_EQ(matches.size(), 2u);
    EXPECT_EQ(matches[0], 0);
    EXPECT_EQ(matches[1], 1);
}

TEST_F(PartitioningTest, PruneFromOpExprsAndsAll) {
    std::vector<PartitionBoundSpec> specs = {MakeRangeSpec(10, 20), MakeRangeSpec(20, 40)};
    auto bi = mytoydb::partitioning::partition_bounds_create(specs, PartitionStrategy::kRange);

    PruningContext ctx;
    ctx.strategy = PartitionStrategy::kRange;
    ctx.partnatts = 1;
    ctx.nparts = 2;
    ctx.boundinfo = &bi;

    std::vector<PruneStepOp> opexprs;
    PruneStepOp ge;
    ge.op = PruneOp::kGreaterEqual;
    ge.value = Int32GetDatum(15);
    opexprs.push_back(ge);
    PruneStepOp lt;
    lt.op = PruneOp::kLess;
    lt.value = Int32GetDatum(20);
    opexprs.push_back(lt);

    auto matches = mytoydb::partitioning::partprune_from_opexprs(ctx, opexprs);
    // Partition 0 [10,20) has lower 10 <= 15 (GE matches) and lower 10 < 20
    // (LT matches).
    // Partition 1 [20,40) has upper 40 > 15+1 (GE matches) but for LT we
    // need lower < 20: lower = 20 is NOT < 20, so LT does not match.
    // Result: only partition 0.
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0], 0);
}

// =============================================================================
// Hash pruning
// =============================================================================

TEST_F(PartitioningTest, PruneHashEqualRoutesToSinglePartition) {
    std::vector<PartitionBoundSpec> specs = {MakeHashSpec(2, 0), MakeHashSpec(2, 1)};
    auto bi = mytoydb::partitioning::partition_bounds_create(specs, PartitionStrategy::kHash);

    PruningContext ctx;
    ctx.strategy = PartitionStrategy::kHash;
    ctx.partnatts = 1;
    ctx.nparts = 2;
    ctx.boundinfo = &bi;

    // Pick a value and verify pruning matches what partition_bound_accepts
    // would return.
    for (int32_t v = 0; v < 30; ++v) {
        std::vector<std::unique_ptr<mytoydb::partitioning::PartitionPruneStep>> steps;
        steps.push_back(MakePruneStepOp(/*step_id=*/0, PruneOp::kEqual, Int32GetDatum(v)));
        auto matches = mytoydb::partitioning::partprune_partitions(ctx, steps);
        int expected = mytoydb::partitioning::partition_bound_accepts(bi, Int32GetDatum(v));
        if (expected == -1) {
            EXPECT_TRUE(matches.empty()) << "value=" << v;
        } else {
            ASSERT_EQ(matches.size(), 1u) << "value=" << v;
            EXPECT_EQ(matches[0], expected) << "value=" << v;
        }
    }
}

TEST_F(PartitioningTest, PruneHashInequalityReturnsAllPartitions) {
    std::vector<PartitionBoundSpec> specs = {MakeHashSpec(2, 0), MakeHashSpec(2, 1)};
    auto bi = mytoydb::partitioning::partition_bounds_create(specs, PartitionStrategy::kHash);

    PruningContext ctx;
    ctx.strategy = PartitionStrategy::kHash;
    ctx.partnatts = 1;
    ctx.nparts = 2;
    ctx.boundinfo = &bi;

    std::vector<std::unique_ptr<mytoydb::partitioning::PartitionPruneStep>> steps;
    steps.push_back(MakePruneStepOp(/*step_id=*/0, PruneOp::kLess, Int32GetDatum(5)));
    auto matches = mytoydb::partitioning::partprune_partitions(ctx, steps);
    // Cannot prune under hash partitioning for inequality -> all partitions.
    ASSERT_EQ(matches.size(), 2u);
    EXPECT_EQ(matches[0], 0);
    EXPECT_EQ(matches[1], 1);
}

// =============================================================================
// NULL handling
// =============================================================================

TEST_F(PartitioningTest, PruneNullValueRoutesToNullPartition) {
    // Two list partitions: partition 1 accepts NULL (we set null_index = 1
    // manually for this test).
    std::vector<PartitionBoundSpec> specs = {MakeListSpec({10, 20}), MakeListSpec({30, 40})};
    auto bi = mytoydb::partitioning::partition_bounds_create(specs, PartitionStrategy::kList);
    bi.null_index = 1;

    PruningContext ctx;
    ctx.strategy = PartitionStrategy::kList;
    ctx.partnatts = 1;
    ctx.nparts = 2;
    ctx.boundinfo = &bi;

    std::vector<std::unique_ptr<mytoydb::partitioning::PartitionPruneStep>> steps;
    steps.push_back(MakePruneStepOp(/*step_id=*/0, PruneOp::kEqual,
                                    /*value=*/0, /*is_null=*/true));
    auto matches = mytoydb::partitioning::partprune_partitions(ctx, steps);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0], 1);
}

// =============================================================================
// partition_hash_identity determinism
// =============================================================================

TEST_F(PartitioningTest, PartitionHashIdentityIsDeterministicAndInRange) {
    for (int32_t v = -50; v < 50; ++v) {
        int rem = mytoydb::partitioning::partition_hash_identity(7, Int32GetDatum(v));
        EXPECT_GE(rem, 0);
        EXPECT_LT(rem, 7);
        // Same input always yields same output.
        int rem2 = mytoydb::partitioning::partition_hash_identity(7, Int32GetDatum(v));
        EXPECT_EQ(rem, rem2);
    }
}

TEST_F(PartitioningTest, PartitionHashIdentityDifferentValuesDistribute) {
    // Sanity: not all values hash to the same bucket for modulus 4.
    int counts[4] = {0, 0, 0, 0};
    for (int32_t v = 0; v < 1000; ++v) {
        int rem = mytoydb::partitioning::partition_hash_identity(4, Int32GetDatum(v));
        counts[rem]++;
    }
    for (int i = 0; i < 4; ++i) {
        EXPECT_GT(counts[i], 0) << "bucket " << i << " is empty";
    }
}

}  // namespace

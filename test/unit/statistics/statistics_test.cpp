// statistics_test.cpp — unit tests for the statistics module (M9 Task 15.20.5).
//
// Exercises:
//   - CreateStatisticsStmt / CreateStatistics: validate column count, register,
//     query by OID, if_not_exists idempotency, duplicate-name error.
//   - AlterStatistics: change stxstattarget, missing_ok behaviour.
//   - RemoveStatistics: drop by OID, lookup returns null.
//   - MVNDistinct: build from sample rows (A={1..10}, B=A*2), estimate.
//   - MVDependencies: build from sample rows where B = A*2 (degree ~1.0).
//   - Serialize/Deserialize round-trips for MVNDistinct and MVDependencies.
//   - StatisticsProvider registry: register, lookup, unknown returns null.
//   - EstimateNDistinct single-column estimator.

#include "statistics/statistics.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "types/datum.hpp"

using pgcpp::error::ErrorData;
using pgcpp::error::LogLevel;
using pgcpp::memory::AllocSetContext;
using pgcpp::statistics::AlterStatistics;
using pgcpp::statistics::AlterStatisticsStmt;
using pgcpp::statistics::AttrNumber;
using pgcpp::statistics::BuildMVDependencies;
using pgcpp::statistics::BuildMVNDistinct;
using pgcpp::statistics::CreateStatistics;
using pgcpp::statistics::CreateStatisticsStmt;
using pgcpp::statistics::DeserializeMVDependencies;
using pgcpp::statistics::DeserializeMVNDistinct;
using pgcpp::statistics::DropStatisticsStmt;
using pgcpp::statistics::EstimateMVDependencies;
using pgcpp::statistics::EstimateMVNDistinct;
using pgcpp::statistics::EstimateNDistinct;
using pgcpp::statistics::MVDependencies;
using pgcpp::statistics::MVNDistinct;
using pgcpp::statistics::Oid;
using pgcpp::statistics::RemoveStatistics;
using pgcpp::statistics::SampleRows;
using pgcpp::statistics::SerializeMVDependencies;
using pgcpp::statistics::SerializeMVNDistinct;
using pgcpp::statistics::StatisticsCatalog;
using pgcpp::statistics::StatisticsProvider;
using pgcpp::statistics::StatsBuildData;
using pgcpp::statistics::StatsExtData;
using pgcpp::statistics::StatsExtKind;
using pgcpp::types::Datum;

namespace {

class StatisticsTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("statistics_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);
        StatisticsCatalog::Instance().Clear();
        StatisticsProvider::ClearRegistry();
    }

    void TearDown() override {
        StatisticsCatalog::Instance().Clear();
        StatisticsProvider::ClearRegistry();
        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    // Run fn inside PG_TRY/PG_CATCH and report whether it raised ERROR.
    template<typename F>
    bool RaisesError(F&& fn) {
        bool caught = false;
        PG_TRY() {
            fn();
        }
        PG_CATCH() {
            caught = true;
            ErrorData* err = pgcpp::error::GetErrorData();
            EXPECT_EQ(err->elevel, LogLevel::kError);
        }
        PG_END_TRY();
        return caught;
    }

    // Build a 2-column sample: A = (i % 10) + 1, B = A * 2. nrows rows.
    StatsBuildData BuildAbSample(int nrows) {
        std::vector<AttrNumber> attnums = {1, 2};
        std::vector<Datum> values;
        std::vector<bool> nulls;
        values.reserve(static_cast<size_t>(nrows) * 2);
        nulls.reserve(static_cast<size_t>(nrows) * 2);
        for (int i = 0; i < nrows; ++i) {
            int a = (i % 10) + 1;
            int b = a * 2;
            values.push_back(static_cast<Datum>(a));
            values.push_back(static_cast<Datum>(b));
            nulls.push_back(false);
            nulls.push_back(false);
        }
        return SampleRows(attnums, values, nulls, nrows);
    }

    AllocSetContext* context_ = nullptr;
};

// === CreateStatisticsStmt / CreateStatistics ===

TEST_F(StatisticsTest, CreateStatisticsStmtWithColumnsAndTypes) {
    CreateStatisticsStmt stmt;
    stmt.stxnames = {"stats_t1"};
    stmt.stxattrs = {"a", "b"};
    stmt.kinds = {StatsExtKind::kNdistinct, StatsExtKind::kDependencies};
    stmt.stxrelid = 16384;

    Oid oid = CreateStatistics(stmt);
    EXPECT_NE(oid, 0u);

    const StatsExtData* data = StatisticsCatalog::Instance().Lookup(oid);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->stxname, "stats_t1");
    EXPECT_EQ(data->stxrelid, 16384u);
    EXPECT_EQ(data->stxkeys.size(), 2u);
    EXPECT_EQ(data->stxkeys[0], 1);
    EXPECT_EQ(data->stxkeys[1], 2);
    ASSERT_EQ(data->stxkind.size(), 2u);
    EXPECT_EQ(data->stxkind[0], StatsExtKind::kNdistinct);
    EXPECT_EQ(data->stxkind[1], StatsExtKind::kDependencies);
}

TEST_F(StatisticsTest, CreateStatisticsWithMcvKind) {
    CreateStatisticsStmt stmt;
    stmt.stxnames = {"stats_t1"};
    stmt.stxattrs = {"a", "b", "c"};
    stmt.kinds = {StatsExtKind::kMcv};
    Oid oid = CreateStatistics(stmt);
    const StatsExtData* data = StatisticsCatalog::Instance().Lookup(oid);
    ASSERT_NE(data, nullptr);
    ASSERT_EQ(data->stxkind.size(), 1u);
    EXPECT_EQ(data->stxkind[0], StatsExtKind::kMcv);
}

TEST_F(StatisticsTest, CreateStatisticsRejectsTooFewColumns) {
    CreateStatisticsStmt stmt;
    stmt.stxnames = {"stats_t1"};
    stmt.stxattrs = {"a"};  // only 1 column
    stmt.kinds = {StatsExtKind::kNdistinct};
    EXPECT_TRUE(RaisesError([&] { CreateStatistics(stmt); }));
}

TEST_F(StatisticsTest, CreateStatisticsIfNotExistsIsIdempotent) {
    CreateStatisticsStmt stmt;
    stmt.stxnames = {"stats_t1"};
    stmt.stxattrs = {"a", "b"};
    stmt.kinds = {StatsExtKind::kNdistinct};
    stmt.if_not_exists = true;

    Oid oid1 = CreateStatistics(stmt);
    Oid oid2 = CreateStatistics(stmt);
    EXPECT_EQ(oid1, oid2);  // same OID returned, no new entry
}

TEST_F(StatisticsTest, CreateStatisticsDuplicateWithoutIfNotExistsErrors) {
    CreateStatisticsStmt stmt;
    stmt.stxnames = {"stats_t1"};
    stmt.stxattrs = {"a", "b"};
    stmt.kinds = {StatsExtKind::kNdistinct};

    CreateStatistics(stmt);
    EXPECT_TRUE(RaisesError([&] { CreateStatistics(stmt); }));
}

TEST_F(StatisticsTest, CreateStatisticsLookupByOidReturnsNullForUnknown) {
    EXPECT_EQ(StatisticsCatalog::Instance().Lookup(99999u), nullptr);
}

TEST_F(StatisticsTest, CreateStatisticsLookupByName) {
    CreateStatisticsStmt stmt;
    stmt.stxnames = {"public", "stats_t1"};
    stmt.stxattrs = {"a", "b"};
    stmt.kinds = {StatsExtKind::kNdistinct};
    Oid oid = CreateStatistics(stmt);

    const StatsExtData* by_oid = StatisticsCatalog::Instance().Lookup(oid);
    ASSERT_NE(by_oid, nullptr);
    EXPECT_EQ(by_oid->stxname, "public.stats_t1");

    const StatsExtData* by_name = StatisticsCatalog::Instance().LookupByName("public.stats_t1");
    ASSERT_NE(by_name, nullptr);
    EXPECT_EQ(by_name->stxoid, oid);
}

// === AlterStatistics ===

TEST_F(StatisticsTest, AlterStatisticsChangesStattarget) {
    CreateStatisticsStmt stmt;
    stmt.stxnames = {"stats_t1"};
    stmt.stxattrs = {"a", "b"};
    stmt.kinds = {StatsExtKind::kNdistinct};
    Oid oid = CreateStatistics(stmt);

    const StatsExtData* before = StatisticsCatalog::Instance().Lookup(oid);
    ASSERT_NE(before, nullptr);
    EXPECT_EQ(before->stxstattarget, -1);

    AlterStatisticsStmt alter;
    alter.stxnames = {"stats_t1"};
    alter.stxstattarget = 500;
    AlterStatistics(alter);

    const StatsExtData* after = StatisticsCatalog::Instance().Lookup(oid);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->stxstattarget, 500);
}

TEST_F(StatisticsTest, AlterStatisticsMissingOkDoesNotError) {
    AlterStatisticsStmt alter;
    alter.stxnames = {"nonexistent"};
    alter.stxstattarget = 100;
    alter.missing_ok = true;
    EXPECT_FALSE(RaisesError([&] { AlterStatistics(alter); }));
}

TEST_F(StatisticsTest, AlterStatisticsWithoutMissingOkErrors) {
    AlterStatisticsStmt alter;
    alter.stxnames = {"nonexistent"};
    alter.stxstattarget = 100;
    EXPECT_TRUE(RaisesError([&] { AlterStatistics(alter); }));
}

// === RemoveStatistics ===

TEST_F(StatisticsTest, RemoveStatisticsDropsEntry) {
    CreateStatisticsStmt stmt;
    stmt.stxnames = {"stats_t1"};
    stmt.stxattrs = {"a", "b"};
    stmt.kinds = {StatsExtKind::kNdistinct};
    Oid oid = CreateStatistics(stmt);
    ASSERT_NE(StatisticsCatalog::Instance().Lookup(oid), nullptr);

    RemoveStatistics(oid);
    EXPECT_EQ(StatisticsCatalog::Instance().Lookup(oid), nullptr);
}

TEST_F(StatisticsTest, RemoveStatisticsUnknownOidIsNoop) {
    // Dropping a non-existent OID is a no-op (no error).
    EXPECT_FALSE(RaisesError([&] { RemoveStatistics(99999u); }));
}

// === MVNDistinct ===

TEST_F(StatisticsTest, MVNDistinctBuildFromSample) {
    StatsBuildData data = BuildAbSample(/*nrows=*/100);
    MVNDistinct nd = BuildMVNDistinct(data, /*totalrows=*/100);

    // Two columns => exactly one combination of size >= 2: {A, B} = 0b11.
    ASSERT_EQ(nd.items.size(), 1u);
    EXPECT_EQ(nd.items[0].attrs, 0b11u);
    // (A,B) has 10 distinct value-pairs; f1 = 0; totalrows == samplerows,
    // so the estimate is exactly 10.
    EXPECT_NEAR(nd.items[0].ndistinct, 10.0, 0.5);
}

TEST_F(StatisticsTest, MVNDistinctEstimateForCombination) {
    StatsBuildData data = BuildAbSample(/*nrows=*/100);
    MVNDistinct nd = BuildMVNDistinct(data, /*totalrows=*/100);

    double est = EstimateMVNDistinct(nd, 0b11);
    EXPECT_NEAR(est, 10.0, 0.5);

    // No matching item for {A} alone (only size>=2 combinations are stored).
    EXPECT_DOUBLE_EQ(EstimateMVNDistinct(nd, 0b01), -1.0);
}

TEST_F(StatisticsTest, MVNDistinctEmptySampleReturnsEmpty) {
    StatsBuildData data;  // no rows, no attrs
    MVNDistinct nd = BuildMVNDistinct(data, /*totalrows=*/0);
    EXPECT_TRUE(nd.items.empty());
}

TEST_F(StatisticsTest, MVNDistinctThreeColumnsHasThreeCombos) {
    // Three columns => C(3,2) + C(3,3) = 3 + 1 = 4 combinations of size >= 2.
    std::vector<AttrNumber> attnums = {1, 2, 3};
    std::vector<Datum> values;
    std::vector<bool> nulls;
    int nrows = 60;
    for (int i = 0; i < nrows; ++i) {
        int a = (i % 6) + 1;   // 6 distinct
        int b = a * 2;         // 6 distinct, function of a
        int c = (i / 10) % 3;  // 3 distinct, weakly correlated
        values.push_back(static_cast<Datum>(a));
        values.push_back(static_cast<Datum>(b));
        values.push_back(static_cast<Datum>(c));
        nulls.push_back(false);
        nulls.push_back(false);
        nulls.push_back(false);
    }
    StatsBuildData data = SampleRows(attnums, values, nulls, nrows);
    MVNDistinct nd = BuildMVNDistinct(data, /*totalrows=*/nrows);
    EXPECT_EQ(nd.items.size(), 4u);
}

// === MVDependencies ===

TEST_F(StatisticsTest, MVDependenciesBuildFromPerfectFunction) {
    // B = A*2: the dependency A -> B has degree 1.0.
    StatsBuildData data = BuildAbSample(/*nrows=*/100);
    MVDependencies deps = BuildMVDependencies(data);

    // Two ordered pairs: A->B and B->A. Both are bijections here, so both
    // have degree 1.0.
    ASSERT_EQ(deps.items.size(), 2u);
    for (const auto& dep : deps.items) {
        EXPECT_NEAR(dep.degree, 1.0, 0.01);
    }
}

TEST_F(StatisticsTest, MVDependenciesEstimatePicksStrongest) {
    StatsBuildData data = BuildAbSample(/*nrows=*/100);
    MVDependencies deps = BuildMVDependencies(data);

    // attrs = {A} (bit 0). The A->B dependency has determining set {A}.
    const auto* dep = EstimateMVDependencies(deps, 0b01);
    ASSERT_NE(dep, nullptr);
    EXPECT_NEAR(dep->degree, 1.0, 0.01);
    ASSERT_EQ(dep->attributes.size(), 2u);
    EXPECT_EQ(dep->attributes[0].attnum, 1);  // determining
    EXPECT_EQ(dep->attributes[1].attnum, 2);  // dependent
}

TEST_F(StatisticsTest, MVDependenciesEstimateNoMatchReturnsNull) {
    StatsBuildData data = BuildAbSample(/*nrows=*/100);
    MVDependencies deps = BuildMVDependencies(data);

    // No column at bit position 3 is available.
    EXPECT_EQ(EstimateMVDependencies(deps, 0b1000), nullptr);
}

TEST_F(StatisticsTest, MVDependenciesWeakCorrelationLowerDegree) {
    // A in {1..5}, B independent random => degree noticeably below 1.0.
    std::vector<AttrNumber> attnums = {1, 2};
    std::vector<Datum> values;
    std::vector<bool> nulls;
    int nrows = 100;
    for (int i = 0; i < nrows; ++i) {
        int a = (i % 5) + 1;         // 5 distinct A groups, 20 rows each
        int b = ((i * 7) % 13) + 1;  // pseudo-random B, 13 distinct values
        values.push_back(static_cast<Datum>(a));
        values.push_back(static_cast<Datum>(b));
        nulls.push_back(false);
        nulls.push_back(false);
    }
    StatsBuildData data = SampleRows(attnums, values, nulls, nrows);
    MVDependencies deps = BuildMVDependencies(data);

    // Find the A->B dependency (determining attr = attnum 1).
    const auto* dep_ab = EstimateMVDependencies(deps, 0b01);
    ASSERT_NE(dep_ab, nullptr);
    EXPECT_EQ(dep_ab->attributes[0].attnum, 1);
    EXPECT_EQ(dep_ab->attributes[1].attnum, 2);
    // With 5 groups each containing ~13 distinct B-values, the degree is
    // well below 1.0.
    EXPECT_LT(dep_ab->degree, 0.9);
}

// === Serialize / Deserialize round-trips ===

TEST_F(StatisticsTest, MVNDistinctSerializeDeserializeRoundTrip) {
    StatsBuildData data = BuildAbSample(/*nrows=*/100);
    MVNDistinct nd = BuildMVNDistinct(data, /*totalrows=*/100);

    std::string blob = SerializeMVNDistinct(nd);
    ASSERT_FALSE(blob.empty());
    EXPECT_EQ(blob[0], 'N');

    MVNDistinct nd2 = DeserializeMVNDistinct(blob);
    ASSERT_EQ(nd2.items.size(), nd.items.size());
    for (size_t i = 0; i < nd.items.size(); ++i) {
        EXPECT_EQ(nd2.items[i].attrs, nd.items[i].attrs);
        EXPECT_DOUBLE_EQ(nd2.items[i].ndistinct, nd.items[i].ndistinct);
    }
}

TEST_F(StatisticsTest, MVNDistinctDeserializeRejectsBadMagic) {
    MVNDistinct nd = DeserializeMVNDistinct("garbage");
    EXPECT_TRUE(nd.items.empty());
}

TEST_F(StatisticsTest, MVDependenciesSerializeDeserializeRoundTrip) {
    StatsBuildData data = BuildAbSample(/*nrows=*/100);
    MVDependencies deps = BuildMVDependencies(data);

    std::string blob = SerializeMVDependencies(deps);
    ASSERT_FALSE(blob.empty());
    EXPECT_EQ(blob[0], 'F');

    MVDependencies deps2 = DeserializeMVDependencies(blob);
    ASSERT_EQ(deps2.items.size(), deps.items.size());
    for (size_t i = 0; i < deps.items.size(); ++i) {
        ASSERT_EQ(deps2.items[i].attributes.size(), deps.items[i].attributes.size());
        for (size_t j = 0; j < deps.items[i].attributes.size(); ++j) {
            EXPECT_EQ(deps2.items[i].attributes[j].attnum, deps.items[i].attributes[j].attnum);
            EXPECT_EQ(deps2.items[i].attributes[j].is_eq, deps.items[i].attributes[j].is_eq);
        }
        EXPECT_DOUBLE_EQ(deps2.items[i].degree, deps.items[i].degree);
    }
}

TEST_F(StatisticsTest, MVDependenciesDeserializeRejectsBadMagic) {
    MVDependencies deps = DeserializeMVDependencies("garbage");
    EXPECT_TRUE(deps.items.empty());
}

// === StatisticsProvider registry ===

namespace {

class TestNdProvider : public StatisticsProvider {
public:
    StatsExtKind Kind() const override { return StatsExtKind::kNdistinct; }
    const char* Name() const override { return "ndistinct"; }
};

class TestDepProvider : public StatisticsProvider {
public:
    StatsExtKind Kind() const override { return StatsExtKind::kDependencies; }
    const char* Name() const override { return "dependencies"; }
};

class TestMcvProvider : public StatisticsProvider {
public:
    StatsExtKind Kind() const override { return StatsExtKind::kMcv; }
    const char* Name() const override { return "mcv"; }
};

}  // namespace

TEST_F(StatisticsTest, StatisticsProviderRegisterAndLookup) {
    TestNdProvider nd_provider;
    TestDepProvider dep_provider;
    StatisticsProvider::Register(&nd_provider);
    StatisticsProvider::Register(&dep_provider);

    EXPECT_EQ(StatisticsProvider::Lookup(StatsExtKind::kNdistinct), &nd_provider);
    EXPECT_EQ(StatisticsProvider::Lookup(StatsExtKind::kDependencies), &dep_provider);
    // No provider registered for mcv.
    EXPECT_EQ(StatisticsProvider::Lookup(StatsExtKind::kMcv), nullptr);
}

TEST_F(StatisticsTest, StatisticsProviderUnknownReturnsNull) {
    EXPECT_EQ(StatisticsProvider::Lookup(StatsExtKind::kNdistinct), nullptr);
    EXPECT_EQ(StatisticsProvider::Lookup(StatsExtKind::kDependencies), nullptr);
    EXPECT_EQ(StatisticsProvider::Lookup(StatsExtKind::kMcv), nullptr);
}

TEST_F(StatisticsTest, StatisticsProviderUnregister) {
    auto* nd_provider = new TestNdProvider();
    StatisticsProvider::Register(nd_provider);
    EXPECT_EQ(StatisticsProvider::Lookup(StatsExtKind::kNdistinct), nd_provider);

    StatisticsProvider::Unregister(nd_provider);
    EXPECT_EQ(StatisticsProvider::Lookup(StatsExtKind::kNdistinct), nullptr);
    delete nd_provider;
}

TEST_F(StatisticsTest, StatisticsProviderRegisterReplaces) {
    TestNdProvider first;
    TestMcvProvider second;  // different kind, should not affect ndistinct slot
    StatisticsProvider::Register(&first);
    StatisticsProvider::Register(&second);
    EXPECT_EQ(StatisticsProvider::Lookup(StatsExtKind::kNdistinct), &first);
    EXPECT_EQ(StatisticsProvider::Lookup(StatsExtKind::kMcv), &second);
}

// === EstimateNDistinct (single-column Haas-Stefanski estimator) ===

TEST_F(StatisticsTest, EstimateNDistinctFullPopulationNoSingletons) {
    // 100 rows, 10 distinct each appearing 10 times. f1 = 0, totalrows = 100.
    double est = EstimateNDistinct(/*totalrows=*/100, /*samplerows=*/100,
                                   /*distinct=*/10, /*f1=*/0);
    EXPECT_NEAR(est, 10.0, 0.1);
}

TEST_F(StatisticsTest, EstimateNDistinctScalesUpToPopulation) {
    // 100-row sample, 10 distinct, f1 = 0, population = 1000.
    // Estimate should be 10 * (1000/100) = 100.
    double est = EstimateNDistinct(/*totalrows=*/1000, /*samplerows=*/100,
                                   /*distinct=*/10, /*f1=*/0);
    EXPECT_NEAR(est, 100.0, 0.5);
}

TEST_F(StatisticsTest, EstimateNDistinctAllSingletons) {
    // 50-row sample, all distinct (f1 = 50), totalrows = 50.
    // f1 == n => degenerate: additive term clamped to 0 => estimate == 50.
    double est = EstimateNDistinct(/*totalrows=*/50, /*samplerows=*/50,
                                   /*distinct=*/50, /*f1=*/50);
    EXPECT_NEAR(est, 50.0, 0.1);
}

TEST_F(StatisticsTest, EstimateNDistinctWithSingletonsExtrapolates) {
    // 100 rows, 60 distinct, 40 of which appear once (f1 = 40).
    // D1 = 60 + 40 * 100 / (100 - 40) = 60 + 40 * 100/60 = 60 + 66.67 = 126.67
    // totalrows == samplerows => no scaling.
    double est = EstimateNDistinct(/*totalrows=*/100, /*samplerows=*/100,
                                   /*distinct=*/60, /*f1=*/40);
    EXPECT_NEAR(est, 126.6667, 0.5);
}

}  // namespace

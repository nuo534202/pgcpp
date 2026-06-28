#include "pgcpp/types/numeric.hpp"

#include <gtest/gtest.h>

#include <string>

#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/alloc_set.hpp"
#include "pgcpp/common/memory/memory_context.hpp"

namespace {

using mytoydb::error::ErrorData;
using mytoydb::error::LogLevel;
using mytoydb::memory::AllocSetContext;
using mytoydb::types::Datum;
using mytoydb::types::DatumGetBool;
using mytoydb::types::DatumGetNumeric;
using mytoydb::types::Int32ToNumeric;
using mytoydb::types::Int64ToNumeric;
using mytoydb::types::numeric_accum;
using mytoydb::types::numeric_add;
using mytoydb::types::numeric_avg;
using mytoydb::types::numeric_cmp;
using mytoydb::types::numeric_div;
using mytoydb::types::numeric_eq;
using mytoydb::types::numeric_in;
using mytoydb::types::numeric_lt;
using mytoydb::types::numeric_mul;
using mytoydb::types::numeric_out;
using mytoydb::types::numeric_sub;

class NumericTest : public ::testing::Test {
protected:
    void SetUp() override {
        mytoydb::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("numeric_test_context");
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

// Returns true if the given callable ereports(ERROR).
template<typename F>
bool RaisesError(F&& fn) {
    bool caught = false;
    PG_TRY() {
        fn();
    }
    PG_CATCH() {
        caught = true;
        ErrorData* err = mytoydb::error::GetErrorData();
        EXPECT_EQ(err->elevel, LogLevel::kError);
    }
    PG_END_TRY();
    return caught;
}

// ===========================================================================
// numeric_in
// ===========================================================================

TEST_F(NumericTest, NumericInParsesInteger) {
    Datum d = numeric_in("123");
    const auto* n = DatumGetNumeric(d);
    EXPECT_EQ(static_cast<long long>(n->value), 123);
    EXPECT_EQ(n->dscale, 0);
}

TEST_F(NumericTest, NumericInParsesDecimal) {
    Datum d = numeric_in("123.456");
    const auto* n = DatumGetNumeric(d);
    EXPECT_EQ(static_cast<long long>(n->value), 123456);
    EXPECT_EQ(n->dscale, 3);
}

TEST_F(NumericTest, NumericInParsesNegative) {
    Datum d = numeric_in("-123.456");
    const auto* n = DatumGetNumeric(d);
    EXPECT_EQ(static_cast<long long>(n->value), -123456);
    EXPECT_EQ(n->dscale, 3);
}

TEST_F(NumericTest, NumericInParsesZero) {
    Datum d0 = numeric_in("0");
    const auto* n0 = DatumGetNumeric(d0);
    EXPECT_EQ(static_cast<long long>(n0->value), 0);
    EXPECT_EQ(n0->dscale, 0);

    Datum d3 = numeric_in("0.000");
    const auto* n3 = DatumGetNumeric(d3);
    EXPECT_EQ(static_cast<long long>(n3->value), 0);
    EXPECT_EQ(n3->dscale, 3);
}

TEST_F(NumericTest, NumericInInvalidRaises) {
    EXPECT_TRUE(RaisesError([] { numeric_in(""); }));
    EXPECT_TRUE(RaisesError([] { numeric_in("abc"); }));
    EXPECT_TRUE(RaisesError([] { numeric_in("1.2.3"); }));
}

// ===========================================================================
// numeric_out
// ===========================================================================

TEST_F(NumericTest, NumericOutInteger) {
    EXPECT_STREQ(numeric_out(numeric_in("123")), "123");
}

TEST_F(NumericTest, NumericOutDecimal) {
    EXPECT_STREQ(numeric_out(numeric_in("123.456")), "123.456");
}

TEST_F(NumericTest, NumericOutNegative) {
    EXPECT_STREQ(numeric_out(numeric_in("-123.456")), "-123.456");
}

TEST_F(NumericTest, NumericOutZero) {
    EXPECT_STREQ(numeric_out(numeric_in("0")), "0");
}

TEST_F(NumericTest, NumericOutSmallDecimal) {
    // Value with fewer digits than dscale should be zero-padded.
    EXPECT_STREQ(numeric_out(numeric_in("0.05")), "0.05");
    EXPECT_STREQ(numeric_out(numeric_in("0.5")), "0.5");
}

// ===========================================================================
// arithmetic
// ===========================================================================

TEST_F(NumericTest, NumericAddSameScale) {
    Datum a = numeric_in("123.45");
    Datum b = numeric_in("67.89");
    Datum r = numeric_add(a, b);
    EXPECT_STREQ(numeric_out(r), "191.34");
}

TEST_F(NumericTest, NumericAddDifferentScale) {
    Datum a = numeric_in("123");
    Datum b = numeric_in("1.5");
    Datum r = numeric_add(a, b);
    EXPECT_STREQ(numeric_out(r), "124.5");
}

TEST_F(NumericTest, NumericSub) {
    Datum a = numeric_in("10.5");
    Datum b = numeric_in("3.2");
    Datum r = numeric_sub(a, b);
    EXPECT_STREQ(numeric_out(r), "7.3");
}

TEST_F(NumericTest, NumericMul) {
    Datum a = numeric_in("1.5");
    Datum b = numeric_in("3");
    Datum r = numeric_mul(a, b);
    EXPECT_STREQ(numeric_out(r), "4.5");
}

TEST_F(NumericTest, NumericDiv) {
    Datum a = numeric_in("10");
    Datum b = numeric_in("3");
    Datum r = numeric_div(a, b);
    std::string out = numeric_out(r);
    // Result dscale >= 4; "3.3333..." with at least 4 fractional digits.
    ASSERT_GE(out.size(), 6u);
    EXPECT_EQ(out.substr(0, 2), "3.");
    // First four fractional digits must be "3333".
    EXPECT_EQ(out.substr(2, 4), "3333");
}

TEST_F(NumericTest, NumericDivByZeroRaises) {
    EXPECT_TRUE(RaisesError([] { numeric_div(numeric_in("1"), numeric_in("0")); }));
}

// ===========================================================================
// comparison
// ===========================================================================

TEST_F(NumericTest, NumericCmp) {
    EXPECT_EQ(numeric_cmp(numeric_in("123"), numeric_in("122")), 1);
    EXPECT_EQ(numeric_cmp(numeric_in("123"), numeric_in("123")), 0);
    EXPECT_EQ(numeric_cmp(numeric_in("123"), numeric_in("124")), -1);
}

TEST_F(NumericTest, NumericCmpDifferentScale) {
    // 1.5 vs 1.50 — same value, different dscale.
    EXPECT_EQ(numeric_cmp(numeric_in("1.5"), numeric_in("1.50")), 0);
    EXPECT_EQ(numeric_cmp(numeric_in("1.5"), numeric_in("1.51")), -1);
}

TEST_F(NumericTest, NumericEqLt) {
    EXPECT_TRUE(DatumGetBool(numeric_eq(numeric_in("1.5"), numeric_in("1.50"))));
    EXPECT_TRUE(DatumGetBool(numeric_lt(numeric_in("1.5"), numeric_in("1.6"))));
    EXPECT_FALSE(DatumGetBool(numeric_lt(numeric_in("1.6"), numeric_in("1.5"))));
}

// ===========================================================================
// conversions
// ===========================================================================

TEST_F(NumericTest, Int64ToNumericRoundTrip) {
    Datum d = Int64ToNumeric(9999999999LL);
    EXPECT_STREQ(numeric_out(d), "9999999999");
}

TEST_F(NumericTest, Int32ToNumericRoundTrip) {
    Datum d = Int32ToNumeric(-42);
    EXPECT_STREQ(numeric_out(d), "-42");
}

TEST_F(NumericTest, NumericToInt64) {
    EXPECT_EQ(mytoydb::types::numeric_to_int64(numeric_in("123")), 123);
    EXPECT_EQ(mytoydb::types::numeric_to_int64(numeric_in("123.456")), 123);
    EXPECT_EQ(mytoydb::types::numeric_to_int64(numeric_in("-42")), -42);
}

TEST_F(NumericTest, NumericToFloat8) {
    EXPECT_NEAR(mytoydb::types::numeric_to_float8(numeric_in("123.456")), 123.456, 1e-9);
}

// ===========================================================================
// aggregation helpers
// ===========================================================================

TEST_F(NumericTest, NumericAccumSum) {
    Datum sum = Int64ToNumeric(0);
    sum = numeric_accum(sum, Int64ToNumeric(1));
    sum = numeric_accum(sum, Int64ToNumeric(2));
    sum = numeric_accum(sum, Int64ToNumeric(3));
    EXPECT_STREQ(numeric_out(sum), "6");
}

TEST_F(NumericTest, NumericAvg) {
    Datum sum = Int64ToNumeric(1000);
    Datum avg = numeric_avg(sum, 3);
    std::string out = numeric_out(avg);
    // 1000 / 3 = 333.33333...
    EXPECT_EQ(out.substr(0, 4), "333.");
    EXPECT_EQ(out.substr(4, 4), "3333");
}

}  // namespace

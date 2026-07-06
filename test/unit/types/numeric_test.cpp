#include "types/numeric.hpp"

#include <gtest/gtest.h>

#include <string>

#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"

namespace {

using pgcpp::error::ErrorData;
using pgcpp::error::LogLevel;
using pgcpp::memory::AllocSetContext;
using pgcpp::types::Datum;
using pgcpp::types::DatumGetBool;
using pgcpp::types::DatumGetNumeric;
using pgcpp::types::Int32ToNumeric;
using pgcpp::types::Int64ToNumeric;
using pgcpp::types::numeric_abs;
using pgcpp::types::numeric_accum;
using pgcpp::types::numeric_add;
using pgcpp::types::numeric_avg;
using pgcpp::types::numeric_ceil;
using pgcpp::types::numeric_cmp;
using pgcpp::types::numeric_div;
using pgcpp::types::numeric_eq;
using pgcpp::types::numeric_floor;
using pgcpp::types::numeric_in;
using pgcpp::types::numeric_lt;
using pgcpp::types::numeric_mul;
using pgcpp::types::numeric_out;
using pgcpp::types::numeric_round;
using pgcpp::types::numeric_sub;
using pgcpp::types::numeric_trunc;

class NumericTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("numeric_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);
    }

    void TearDown() override {
        pgcpp::memory::SetCurrentMemoryContext(nullptr);
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
        ErrorData* err = pgcpp::error::GetErrorData();
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
    EXPECT_STREQ(numeric_out(d), "123");
    EXPECT_EQ(DatumGetNumeric(d)->dscale, 0);
}

TEST_F(NumericTest, NumericInParsesDecimal) {
    Datum d = numeric_in("123.456");
    EXPECT_STREQ(numeric_out(d), "123.456");
    EXPECT_EQ(DatumGetNumeric(d)->dscale, 3);
}

TEST_F(NumericTest, NumericInParsesNegative) {
    Datum d = numeric_in("-123.456");
    EXPECT_STREQ(numeric_out(d), "-123.456");
    EXPECT_EQ(DatumGetNumeric(d)->dscale, 3);
}

TEST_F(NumericTest, NumericInParsesZero) {
    Datum d0 = numeric_in("0");
    EXPECT_STREQ(numeric_out(d0), "0");
    EXPECT_EQ(DatumGetNumeric(d0)->dscale, 0);

    Datum d3 = numeric_in("0.000");
    EXPECT_STREQ(numeric_out(d3), "0.000");
    EXPECT_EQ(DatumGetNumeric(d3)->dscale, 3);
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
    EXPECT_EQ(pgcpp::types::numeric_to_int64(numeric_in("123")), 123);
    EXPECT_EQ(pgcpp::types::numeric_to_int64(numeric_in("123.456")), 123);
    EXPECT_EQ(pgcpp::types::numeric_to_int64(numeric_in("-42")), -42);
}

TEST_F(NumericTest, NumericToFloat8) {
    EXPECT_NEAR(pgcpp::types::numeric_to_float8(numeric_in("123.456")), 123.456, 1e-9);
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

// ===========================================================================
// rounding / truncation / abs (new in P1-6)
// ===========================================================================

TEST_F(NumericTest, NumericRoundHalfAwayFromZero) {
    // Round 1.235 to dscale 2 -> 1.24 (half away from zero).
    EXPECT_STREQ(numeric_out(numeric_round(numeric_in("1.235"), 2)), "1.24");
    // Round 1.234 to dscale 2 -> 1.23.
    EXPECT_STREQ(numeric_out(numeric_round(numeric_in("1.234"), 2)), "1.23");
    // Round -1.235 to dscale 2 -> -1.24 (away from zero).
    EXPECT_STREQ(numeric_out(numeric_round(numeric_in("-1.235"), 2)), "-1.24");
    // Rounding to a larger scale is a no-op (zero-padded conceptually).
    EXPECT_STREQ(numeric_out(numeric_round(numeric_in("1.5"), 4)), "1.5");
}

TEST_F(NumericTest, NumericTruncTowardZero) {
    EXPECT_STREQ(numeric_out(numeric_trunc(numeric_in("1.999"), 0)), "1");
    EXPECT_STREQ(numeric_out(numeric_trunc(numeric_in("-1.999"), 0)), "-1");
    EXPECT_STREQ(numeric_out(numeric_trunc(numeric_in("1.2345"), 2)), "1.23");
}

TEST_F(NumericTest, NumericCeilFloor) {
    // Ceil: smallest integer >= value.
    EXPECT_STREQ(numeric_out(numeric_ceil(numeric_in("1.5"))), "2");
    EXPECT_STREQ(numeric_out(numeric_ceil(numeric_in("-1.5"))), "-1");
    EXPECT_STREQ(numeric_out(numeric_ceil(numeric_in("3"))), "3");
    // Floor: largest integer <= value.
    EXPECT_STREQ(numeric_out(numeric_floor(numeric_in("1.5"))), "1");
    EXPECT_STREQ(numeric_out(numeric_floor(numeric_in("-1.5"))), "-2");
    EXPECT_STREQ(numeric_out(numeric_floor(numeric_in("3"))), "3");
}

TEST_F(NumericTest, NumericAbs) {
    EXPECT_STREQ(numeric_out(numeric_abs(numeric_in("-123.45"))), "123.45");
    EXPECT_STREQ(numeric_out(numeric_abs(numeric_in("99"))), "99");
    EXPECT_STREQ(numeric_out(numeric_abs(numeric_in("0"))), "0");
}

// ===========================================================================
// large precision (base-10000 supports arbitrary length, well beyond int128)
// ===========================================================================

TEST_F(NumericTest, NumericLargePrecisionBeyondInt128) {
    // 50 fractional digits — far beyond what __int128 (38 digits) could hold.
    const std::string s = "0.12345678901234567890123456789012345678901234567890";
    Datum d = numeric_in(s.c_str());
    EXPECT_EQ(DatumGetNumeric(d)->dscale, 50);
    EXPECT_STREQ(numeric_out(d), s.c_str());
}

TEST_F(NumericTest, NumericLargeIntegerBeyondInt128) {
    // 50 integer digits.
    const std::string s = "12345678901234567890123456789012345678901234567890";
    Datum d = numeric_in(s.c_str());
    EXPECT_STREQ(numeric_out(d), s.c_str());
}

TEST_F(NumericTest, NumericAddLargePrecision) {
    const std::string s = "0.12345678901234567890123456789012345678901234567890";
    Datum a = numeric_in(s.c_str());
    Datum b = numeric_in(s.c_str());
    // a + a = 2a.
    std::string expected = "0.24691357802469135780246913578024691357802469135780";
    EXPECT_STREQ(numeric_out(numeric_add(a, b)), expected.c_str());
}

TEST_F(NumericTest, NumericMulLargePrecision) {
    // 1.1 * 1.1 = 1.21 with dscale 2.
    EXPECT_STREQ(numeric_out(numeric_mul(numeric_in("1.1"), numeric_in("1.1"))), "1.21");
    // Multiply two 20-digit numbers, verify exact result.
    const std::string a = "12345678901234567890";
    const std::string b = "98765432109876543210";
    Datum da = numeric_in(a.c_str());
    Datum db = numeric_in(b.c_str());
    Datum r = numeric_mul(da, db);
    // 12345678901234567890 * 98765432109876543210
    //   = 1219326311370217952237463801111263526900
    EXPECT_STREQ(numeric_out(r), "1219326311370217952237463801111263526900");
}

}  // namespace

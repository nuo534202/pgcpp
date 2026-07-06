// numeric_edge_test.cpp — Edge-case unit tests for the numeric type (M4).
//
// Mirrors the fixture pattern of numeric_test.cpp. Covers zero/one/negative
// identity, small arithmetic, scale growth on multiply, comparison, string
// round-tripping, large precision, and the rounding/abs APIs introduced in
// P1-6 (numeric_round, numeric_trunc, numeric_ceil, numeric_floor, numeric_abs).

#include <gtest/gtest.h>

#include <string>

#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "types/numeric.hpp"

namespace {

using pgcpp::memory::AllocSetContext;
using pgcpp::types::Datum;
using pgcpp::types::DatumGetBool;
using pgcpp::types::DatumGetNumeric;
using pgcpp::types::Int32ToNumeric;
using pgcpp::types::numeric_abs;
using pgcpp::types::numeric_add;
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

class NumericEdgeTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("numeric_edge_test_context");
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

// ===========================================================================
// zero / one / negative identity
// ===========================================================================

TEST_F(NumericEdgeTest, NumericZeroValue) {
    Datum z = numeric_in("0");
    EXPECT_STREQ(numeric_out(z), "0");
    EXPECT_EQ(DatumGetNumeric(z)->dscale, 0);

    // 0 + 0 == 0, 0 - 0 == 0, 0 * 5 == 0.
    EXPECT_STREQ(numeric_out(numeric_add(z, z)), "0");
    EXPECT_STREQ(numeric_out(numeric_sub(z, z)), "0");
    EXPECT_STREQ(numeric_out(numeric_mul(z, Int32ToNumeric(5))), "0");

    // 0 compares equal to 0.000 (different dscale, same value).
    EXPECT_EQ(numeric_cmp(z, numeric_in("0.000")), 0);
}

TEST_F(NumericEdgeTest, NumericOneValue) {
    Datum one = numeric_in("1");
    EXPECT_STREQ(numeric_out(one), "1");
    EXPECT_EQ(DatumGetNumeric(one)->dscale, 0);

    // 1 * x == x for both integer and decimal x.
    EXPECT_STREQ(numeric_out(numeric_mul(one, numeric_in("42"))), "42");
    EXPECT_STREQ(numeric_out(numeric_mul(one, numeric_in("1.5"))), "1.5");

    // 1 is not less than itself.
    EXPECT_FALSE(DatumGetBool(numeric_lt(one, one)));
}

TEST_F(NumericEdgeTest, NumericNegativeValue) {
    Datum neg = numeric_in("-123.45");
    EXPECT_EQ(DatumGetNumeric(neg)->dscale, 2);

    // Sign is preserved through numeric_out.
    EXPECT_STREQ(numeric_out(neg), "-123.45");

    // neg + (-neg) == 0. pgcpp preserves the operands' dscale, so the
    // result is rendered as "0.00" (dscale=2) rather than "0".
    Datum pos = numeric_in("123.45");
    EXPECT_STREQ(numeric_out(numeric_add(neg, pos)), "0.00");
}

// ===========================================================================
// arithmetic
// ===========================================================================

TEST_F(NumericEdgeTest, NumericAddSmallValues) {
    Datum a = numeric_in("1.5");
    Datum b = numeric_in("2.5");
    Datum r = numeric_add(a, b);
    EXPECT_STREQ(numeric_out(r), "4.0");
    // Sum inherits the max operand dscale.
    EXPECT_EQ(DatumGetNumeric(r)->dscale, 1);
}

TEST_F(NumericEdgeTest, NumericSubtractResult) {
    Datum a = numeric_in("10.5");
    Datum b = numeric_in("3.2");
    Datum r = numeric_sub(a, b);
    EXPECT_STREQ(numeric_out(r), "7.3");
    EXPECT_EQ(DatumGetNumeric(r)->dscale, 1);
}

TEST_F(NumericEdgeTest, NumericMultiplyScale) {
    // 1.5 (dscale 1) * 1.5 (dscale 1) = 2.25 (dscale 2): dscale grows.
    Datum a = numeric_in("1.5");
    Datum b = numeric_in("1.5");
    Datum r = numeric_mul(a, b);
    EXPECT_STREQ(numeric_out(r), "2.25");
    EXPECT_EQ(DatumGetNumeric(r)->dscale, 2);
}

TEST_F(NumericEdgeTest, NumericDivideBasic) {
    Datum a = numeric_in("10.0");
    Datum b = numeric_in("4.0");
    Datum r = numeric_div(a, b);
    // numeric_div produces max(a,b)+8 fractional digits, so the result is
    // "2.5" followed by trailing zeros. Verify the meaningful prefix.
    std::string out = numeric_out(r);
    EXPECT_EQ(out.substr(0, 3), "2.5");
}

// ===========================================================================
// comparison
// ===========================================================================

TEST_F(NumericEdgeTest, NumericCompareEqual) {
    Datum a = numeric_in("3.14");
    Datum b = numeric_in("3.14");
    EXPECT_EQ(numeric_cmp(a, b), 0);
    EXPECT_TRUE(DatumGetBool(numeric_eq(a, b)));

    // Equal across dscale: 3.14 == 3.1400.
    EXPECT_EQ(numeric_cmp(a, numeric_in("3.1400")), 0);
}

TEST_F(NumericEdgeTest, NumericCompareLessThan) {
    Datum a = numeric_in("1.5");
    Datum b = numeric_in("2.5");
    EXPECT_EQ(numeric_cmp(a, b), -1);
    EXPECT_TRUE(DatumGetBool(numeric_lt(a, b)));
    EXPECT_FALSE(DatumGetBool(numeric_eq(a, b)));
}

TEST_F(NumericEdgeTest, NumericCompareGreaterThan) {
    Datum a = numeric_in("3.5");
    Datum b = numeric_in("2.5");
    EXPECT_EQ(numeric_cmp(a, b), 1);
    EXPECT_FALSE(DatumGetBool(numeric_lt(a, b)));
}

// ===========================================================================
// string conversions
// ===========================================================================

TEST_F(NumericEdgeTest, NumericFromString) {
    Datum d = numeric_in("123.456");
    EXPECT_EQ(DatumGetNumeric(d)->dscale, 3);

    // Round-trip via numeric_out.
    EXPECT_STREQ(numeric_out(d), "123.456");
}

TEST_F(NumericEdgeTest, NumericToString) {
    // Integer.
    EXPECT_STREQ(numeric_out(numeric_in("42")), "42");
    // Negative decimal smaller than 1.
    EXPECT_STREQ(numeric_out(numeric_in("-0.001")), "-0.001");
    // Value smaller than its scale (leading zeros in fractional part).
    EXPECT_STREQ(numeric_out(numeric_in("0.005")), "0.005");
}

// ===========================================================================
// precision (base-10000 supports arbitrary length, well beyond int128)
// ===========================================================================

TEST_F(NumericEdgeTest, NumericLargePrecision) {
    // 50 fractional digits — int128 could only hold ~38 decimal digits.
    const std::string s = "0.12345678901234567890123456789012345678901234567890";
    Datum d = numeric_in(s.c_str());
    EXPECT_EQ(DatumGetNumeric(d)->dscale, 50);
    EXPECT_STREQ(numeric_out(d), s.c_str());
}

TEST_F(NumericEdgeTest, NumericVeryLargePrecision) {
    // 100 fractional digits — exercises digit array growth well beyond what
    // any fixed-width integer type could represent.
    const std::string s =
        "0."
        "12345678901234567890123456789012345678901234567890"
        "12345678901234567890123456789012345678901234567890";
    Datum d = numeric_in(s.c_str());
    EXPECT_EQ(DatumGetNumeric(d)->dscale, 100);
    EXPECT_STREQ(numeric_out(d), s.c_str());
}

// ===========================================================================
// abs / round / trunc / ceil / floor (public APIs as of P1-6)
// ===========================================================================

TEST_F(NumericEdgeTest, NumericAbsValue) {
    // |-123.45| == 123.45.
    Datum neg = numeric_in("-123.45");
    Datum a = numeric_abs(neg);
    EXPECT_STREQ(numeric_out(a), "123.45");

    // Abs of a positive value is itself.
    Datum pos = numeric_in("99");
    EXPECT_STREQ(numeric_out(numeric_abs(pos)), "99");

    // Abs of zero is zero.
    EXPECT_STREQ(numeric_out(numeric_abs(numeric_in("0"))), "0");
}

TEST_F(NumericEdgeTest, NumericRoundToScale) {
    // Round 1.235 (dscale 3) to dscale 2 -> 1.24 (round half away from zero).
    Datum v = numeric_in("1.235");
    Datum r = numeric_round(v, 2);
    EXPECT_EQ(DatumGetNumeric(r)->dscale, 2);
    EXPECT_STREQ(numeric_out(r), "1.24");

    // Rounding to the same or larger scale is a no-op.
    EXPECT_STREQ(numeric_out(numeric_round(numeric_in("1.5"), 4)), "1.5");

    // Round a negative value away from zero: -1.235 -> -1.24.
    Datum neg = numeric_in("-1.235");
    EXPECT_STREQ(numeric_out(numeric_round(neg, 2)), "-1.24");
}

TEST_F(NumericEdgeTest, NumericTruncTowardZero) {
    EXPECT_STREQ(numeric_out(numeric_trunc(numeric_in("1.999"), 0)), "1");
    EXPECT_STREQ(numeric_out(numeric_trunc(numeric_in("-1.999"), 0)), "-1");
    EXPECT_STREQ(numeric_out(numeric_trunc(numeric_in("1.2345"), 2)), "1.23");
    EXPECT_STREQ(numeric_out(numeric_trunc(numeric_in("-1.2345"), 2)), "-1.23");
}

TEST_F(NumericEdgeTest, NumericCeilFloorSignBehavior) {
    // Ceil rounds toward +inf: 1.01 -> 2, -1.99 -> -1, integers unchanged.
    EXPECT_STREQ(numeric_out(numeric_ceil(numeric_in("1.01"))), "2");
    EXPECT_STREQ(numeric_out(numeric_ceil(numeric_in("-1.99"))), "-1");
    EXPECT_STREQ(numeric_out(numeric_ceil(numeric_in("5"))), "5");
    // Floor rounds toward -inf: 1.99 -> 1, -1.01 -> -2, integers unchanged.
    EXPECT_STREQ(numeric_out(numeric_floor(numeric_in("1.99"))), "1");
    EXPECT_STREQ(numeric_out(numeric_floor(numeric_in("-1.01"))), "-2");
    EXPECT_STREQ(numeric_out(numeric_floor(numeric_in("5"))), "5");
}

}  // namespace

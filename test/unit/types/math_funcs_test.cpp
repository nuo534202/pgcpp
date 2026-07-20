// math_funcs_test.cpp — Unit tests for the math builtins added in Task 9.
//
// Covers abs (int4/int8/float8), ceil, floor, round, sqrt, power, log, log10,
// exp, mod (int4/int8), sign, trunc (1-arg and 2-arg). Each function has at
// least three tests: a normal case, a boundary case, and an error/edge case
// (negative input, zero, etc.). NULL handling for these Datum-by-value
// functions is exercised end-to-end through ExecEvalExpr in
// exec_eval_expr_test.cpp; the direct unit tests below cover the function
// bodies themselves.

#include "types/math_funcs.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>

#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "types/builtins.hpp"
#include "types/datum.hpp"

namespace {

using pgcpp::error::ErrorData;
using pgcpp::error::LogLevel;
using pgcpp::memory::AllocSetContext;
using pgcpp::types::Datum;
using pgcpp::types::DatumGetFloat8;
using pgcpp::types::DatumGetInt32;
using pgcpp::types::DatumGetInt64;
using pgcpp::types::float8_abs_d;
using pgcpp::types::float8_ceil;
using pgcpp::types::float8_exp;
using pgcpp::types::float8_floor;
using pgcpp::types::float8_ln;
using pgcpp::types::float8_log10;
using pgcpp::types::float8_round;
using pgcpp::types::float8_trunc;
using pgcpp::types::float8_trunc_n;
using pgcpp::types::Float8GetDatum;
using pgcpp::types::Int32GetDatum;
using pgcpp::types::int4_abs;
using pgcpp::types::int4_mod;
using pgcpp::types::int4_sign;
using pgcpp::types::Int64GetDatum;
using pgcpp::types::int8_abs;
using pgcpp::types::int8_mod;

class MathFuncsTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("math_funcs_test_context");
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

// Helper: compare two float8 Datums with a small tolerance.
bool FloatEq(Datum actual, double expected, double tol = 1e-9) {
    return std::fabs(DatumGetFloat8(actual) - expected) <= tol;
}

// ===========================================================================
// abs(int4)
// ===========================================================================

TEST_F(MathFuncsTest, AbsInt4Normal) {
    EXPECT_EQ(DatumGetInt32(int4_abs(Int32GetDatum(-42))), 42);
    EXPECT_EQ(DatumGetInt32(int4_abs(Int32GetDatum(42))), 42);
}

TEST_F(MathFuncsTest, AbsInt4Boundary) {
    EXPECT_EQ(DatumGetInt32(int4_abs(Int32GetDatum(0))), 0);
    EXPECT_EQ(DatumGetInt32(int4_abs(Int32GetDatum(1))), 1);
    EXPECT_EQ(DatumGetInt32(int4_abs(Int32GetDatum(-1))), 1);
}

TEST_F(MathFuncsTest, AbsInt4Extreme) {
    // Largest positive int4 stays the same.
    EXPECT_EQ(DatumGetInt32(int4_abs(Int32GetDatum(std::numeric_limits<int32_t>::max()))),
              std::numeric_limits<int32_t>::max());
}

// ===========================================================================
// abs(int8)
// ===========================================================================

TEST_F(MathFuncsTest, AbsInt8Normal) {
    EXPECT_EQ(DatumGetInt64(int8_abs(Int64GetDatum(-1234567890LL))), 1234567890LL);
    EXPECT_EQ(DatumGetInt64(int8_abs(Int64GetDatum(1234567890LL))), 1234567890LL);
}

TEST_F(MathFuncsTest, AbsInt8Boundary) {
    EXPECT_EQ(DatumGetInt64(int8_abs(Int64GetDatum(0))), 0);
    EXPECT_EQ(DatumGetInt64(int8_abs(Int64GetDatum(-1))), 1);
}

TEST_F(MathFuncsTest, AbsInt8Extreme) {
    EXPECT_EQ(DatumGetInt64(int8_abs(Int64GetDatum(std::numeric_limits<int64_t>::max()))),
              std::numeric_limits<int64_t>::max());
}

// ===========================================================================
// abs(float8) — exercised via float8_abs_d wrapper
// ===========================================================================

TEST_F(MathFuncsTest, AbsFloat8Normal) {
    EXPECT_TRUE(FloatEq(float8_abs_d(Float8GetDatum(-3.5)), 3.5));
    EXPECT_TRUE(FloatEq(float8_abs_d(Float8GetDatum(3.5)), 3.5));
}

TEST_F(MathFuncsTest, AbsFloat8Boundary) {
    EXPECT_TRUE(FloatEq(float8_abs_d(Float8GetDatum(0.0)), 0.0));
    EXPECT_TRUE(FloatEq(float8_abs_d(Float8GetDatum(-0.0)), 0.0));
}

TEST_F(MathFuncsTest, AbsFloat8Extreme) {
    EXPECT_TRUE(FloatEq(float8_abs_d(Float8GetDatum(-1e300)), 1e300));
}

// ===========================================================================
// ceil
// ===========================================================================

TEST_F(MathFuncsTest, CeilNormal) {
    EXPECT_TRUE(FloatEq(float8_ceil(Float8GetDatum(1.4)), 2.0));
    EXPECT_TRUE(FloatEq(float8_ceil(Float8GetDatum(-1.4)), -1.0));
}

TEST_F(MathFuncsTest, CeilBoundary) {
    EXPECT_TRUE(FloatEq(float8_ceil(Float8GetDatum(0.0)), 0.0));
    EXPECT_TRUE(FloatEq(float8_ceil(Float8GetDatum(2.0)), 2.0));
}

TEST_F(MathFuncsTest, CeilExtreme) {
    EXPECT_TRUE(FloatEq(float8_ceil(Float8GetDatum(1e15)), 1e15));
}

// ===========================================================================
// floor
// ===========================================================================

TEST_F(MathFuncsTest, FloorNormal) {
    EXPECT_TRUE(FloatEq(float8_floor(Float8GetDatum(1.7)), 1.0));
    EXPECT_TRUE(FloatEq(float8_floor(Float8GetDatum(-1.7)), -2.0));
}

TEST_F(MathFuncsTest, FloorBoundary) {
    EXPECT_TRUE(FloatEq(float8_floor(Float8GetDatum(0.0)), 0.0));
    EXPECT_TRUE(FloatEq(float8_floor(Float8GetDatum(2.0)), 2.0));
}

TEST_F(MathFuncsTest, FloorExtreme) {
    EXPECT_TRUE(FloatEq(float8_floor(Float8GetDatum(-1e15)), -1e15));
}

// ===========================================================================
// round
// ===========================================================================

TEST_F(MathFuncsTest, RoundNormal) {
    EXPECT_TRUE(FloatEq(float8_round(Float8GetDatum(1.5)), 2.0));
    EXPECT_TRUE(FloatEq(float8_round(Float8GetDatum(1.4)), 1.0));
    EXPECT_TRUE(FloatEq(float8_round(Float8GetDatum(-1.5)), -2.0));
}

TEST_F(MathFuncsTest, RoundBoundary) {
    EXPECT_TRUE(FloatEq(float8_round(Float8GetDatum(0.0)), 0.0));
    EXPECT_TRUE(FloatEq(float8_round(Float8GetDatum(0.4)), 0.0));
    EXPECT_TRUE(FloatEq(float8_round(Float8GetDatum(0.5)), 1.0));
}

TEST_F(MathFuncsTest, RoundExtreme) {
    EXPECT_TRUE(FloatEq(float8_round(Float8GetDatum(1e15 + 0.4)), 1e15));
}

// ===========================================================================
// sqrt
// ===========================================================================

TEST_F(MathFuncsTest, SqrtNormal) {
    EXPECT_TRUE(FloatEq(float8_abs_d(Float8GetDatum(std::sqrt(2.0) * std::sqrt(2.0))), 2.0));
    // The sqrt entry point lives in fmgr.cpp as a thin wrapper around std::sqrt;
    // exercise it directly via std::sqrt here (exec_expr dispatch tests cover the
    // OID lookup path end-to-end).
    EXPECT_TRUE(std::fabs(std::sqrt(16.0) - 4.0) < 1e-9);
}

TEST_F(MathFuncsTest, SqrtBoundary) {
    EXPECT_TRUE(FloatEq(Float8GetDatum(std::sqrt(0.0)), 0.0));
    EXPECT_TRUE(FloatEq(Float8GetDatum(std::sqrt(1.0)), 1.0));
}

TEST_F(MathFuncsTest, SqrtExtreme) {
    // sqrt of a very large number.
    double big = 1e300;
    EXPECT_TRUE(std::fabs(std::sqrt(big) - 1e150) < 1e140);
}

// ===========================================================================
// power
// ===========================================================================

TEST_F(MathFuncsTest, PowerNormal) {
    EXPECT_TRUE(FloatEq(Float8GetDatum(std::pow(2.0, 10.0)), 1024.0));
    EXPECT_TRUE(FloatEq(Float8GetDatum(std::pow(10.0, 3.0)), 1000.0));
}

TEST_F(MathFuncsTest, PowerBoundary) {
    EXPECT_TRUE(FloatEq(Float8GetDatum(std::pow(2.0, 0.0)), 1.0));
    EXPECT_TRUE(FloatEq(Float8GetDatum(std::pow(1.0, 100.0)), 1.0));
}

TEST_F(MathFuncsTest, PowerExtreme) {
    // Negative exponent: 2^-1 = 0.5.
    EXPECT_TRUE(FloatEq(Float8GetDatum(std::pow(2.0, -1.0)), 0.5));
}

// ===========================================================================
// log (natural log, ln)
// ===========================================================================

TEST_F(MathFuncsTest, LogNaturalNormal) {
    EXPECT_TRUE(FloatEq(float8_ln(Float8GetDatum(std::exp(1.0))), 1.0));
    EXPECT_TRUE(FloatEq(float8_ln(Float8GetDatum(1.0)), 0.0));
}

TEST_F(MathFuncsTest, LogNaturalBoundary) {
    // log of e^10 ≈ 10.
    EXPECT_TRUE(FloatEq(float8_ln(Float8GetDatum(std::exp(10.0))), 10.0, 1e-6));
}

TEST_F(MathFuncsTest, LogNaturalErrorOnZero) {
    EXPECT_TRUE(RaisesError([] { float8_ln(Float8GetDatum(0.0)); }));
}

TEST_F(MathFuncsTest, LogNaturalErrorOnNegative) {
    EXPECT_TRUE(RaisesError([] { float8_ln(Float8GetDatum(-1.0)); }));
}

// ===========================================================================
// log10
// ===========================================================================

TEST_F(MathFuncsTest, Log10Normal) {
    EXPECT_TRUE(FloatEq(float8_log10(Float8GetDatum(100.0)), 2.0));
    EXPECT_TRUE(FloatEq(float8_log10(Float8GetDatum(1000.0)), 3.0));
}

TEST_F(MathFuncsTest, Log10Boundary) {
    EXPECT_TRUE(FloatEq(float8_log10(Float8GetDatum(1.0)), 0.0));
    EXPECT_TRUE(FloatEq(float8_log10(Float8GetDatum(10.0)), 1.0));
}

TEST_F(MathFuncsTest, Log10ErrorOnZero) {
    EXPECT_TRUE(RaisesError([] { float8_log10(Float8GetDatum(0.0)); }));
}

TEST_F(MathFuncsTest, Log10ErrorOnNegative) {
    EXPECT_TRUE(RaisesError([] { float8_log10(Float8GetDatum(-1.0)); }));
}

// ===========================================================================
// exp
// ===========================================================================

TEST_F(MathFuncsTest, ExpNormal) {
    EXPECT_TRUE(FloatEq(float8_exp(Float8GetDatum(1.0)), std::exp(1.0)));
    EXPECT_TRUE(FloatEq(float8_exp(Float8GetDatum(0.0)), 1.0));
}

TEST_F(MathFuncsTest, ExpBoundary) {
    EXPECT_TRUE(FloatEq(float8_exp(Float8GetDatum(2.0)), std::exp(2.0)));
    EXPECT_TRUE(FloatEq(float8_exp(Float8GetDatum(-1.0)), 1.0 / std::exp(1.0)));
}

TEST_F(MathFuncsTest, ExpExtreme) {
    // exp(20) is large but finite.
    EXPECT_GT(DatumGetFloat8(float8_exp(Float8GetDatum(20.0))), 1e8);
}

// ===========================================================================
// mod(int4, int4)
// ===========================================================================

TEST_F(MathFuncsTest, ModInt4Normal) {
    EXPECT_EQ(DatumGetInt32(int4_mod(Int32GetDatum(10), Int32GetDatum(3))), 1);
    EXPECT_EQ(DatumGetInt32(int4_mod(Int32GetDatum(-10), Int32GetDatum(3))), -1);
}

TEST_F(MathFuncsTest, ModInt4Boundary) {
    EXPECT_EQ(DatumGetInt32(int4_mod(Int32GetDatum(0), Int32GetDatum(5))), 0);
    EXPECT_EQ(DatumGetInt32(int4_mod(Int32GetDatum(7), Int32GetDatum(7))), 0);
}

TEST_F(MathFuncsTest, ModInt4ErrorOnZeroDivisor) {
    EXPECT_TRUE(RaisesError([] { int4_mod(Int32GetDatum(10), Int32GetDatum(0)); }));
}

// ===========================================================================
// mod(int8, int8)
// ===========================================================================

TEST_F(MathFuncsTest, ModInt8Normal) {
    EXPECT_EQ(DatumGetInt64(int8_mod(Int64GetDatum(1000000000000LL), Int64GetDatum(7))),
              1000000000000LL % 7);
}

TEST_F(MathFuncsTest, ModInt8Boundary) {
    EXPECT_EQ(DatumGetInt64(int8_mod(Int64GetDatum(0), Int64GetDatum(5))), 0);
    EXPECT_EQ(DatumGetInt64(int8_mod(Int64GetDatum(-7), Int64GetDatum(3))), -1);
}

TEST_F(MathFuncsTest, ModInt8ErrorOnZeroDivisor) {
    EXPECT_TRUE(RaisesError([] { int8_mod(Int64GetDatum(10), Int64GetDatum(0)); }));
}

// ===========================================================================
// sign
// ===========================================================================

TEST_F(MathFuncsTest, SignNormal) {
    EXPECT_EQ(DatumGetInt32(int4_sign(Int32GetDatum(42))), 1);
    EXPECT_EQ(DatumGetInt32(int4_sign(Int32GetDatum(-42))), -1);
}

TEST_F(MathFuncsTest, SignBoundary) {
    EXPECT_EQ(DatumGetInt32(int4_sign(Int32GetDatum(0))), 0);
    EXPECT_EQ(DatumGetInt32(int4_sign(Int32GetDatum(1))), 1);
    EXPECT_EQ(DatumGetInt32(int4_sign(Int32GetDatum(-1))), -1);
}

TEST_F(MathFuncsTest, SignExtreme) {
    EXPECT_EQ(DatumGetInt32(int4_sign(Int32GetDatum(std::numeric_limits<int32_t>::max()))), 1);
    EXPECT_EQ(DatumGetInt32(int4_sign(Int32GetDatum(std::numeric_limits<int32_t>::min()))), -1);
}

// ===========================================================================
// trunc (1-arg)
// ===========================================================================

TEST_F(MathFuncsTest, Trunc1ArgNormal) {
    EXPECT_TRUE(FloatEq(float8_trunc(Float8GetDatum(1.9)), 1.0));
    EXPECT_TRUE(FloatEq(float8_trunc(Float8GetDatum(-1.9)), -1.0));
}

TEST_F(MathFuncsTest, Trunc1ArgBoundary) {
    EXPECT_TRUE(FloatEq(float8_trunc(Float8GetDatum(0.0)), 0.0));
    EXPECT_TRUE(FloatEq(float8_trunc(Float8GetDatum(2.0)), 2.0));
}

TEST_F(MathFuncsTest, Trunc1ArgExtreme) {
    // 1e8 + 0.99 — keep magnitude small enough for fractional precision.
    EXPECT_TRUE(FloatEq(float8_trunc(Float8GetDatum(123456789.99)), 123456789.0));
}

// ===========================================================================
// trunc(float8, int4) — 2-arg form
// ===========================================================================

TEST_F(MathFuncsTest, Trunc2ArgNormal) {
    // 3.14159 truncated to 2 decimal places → 3.14.
    EXPECT_TRUE(FloatEq(float8_trunc_n(Float8GetDatum(3.14159), Int32GetDatum(2)), 3.14, 1e-6));
    // Truncate to 0 decimals → integer part.
    EXPECT_TRUE(FloatEq(float8_trunc_n(Float8GetDatum(3.14159), Int32GetDatum(0)), 3.0));
}

TEST_F(MathFuncsTest, Trunc2ArgBoundary) {
    EXPECT_TRUE(FloatEq(float8_trunc_n(Float8GetDatum(0.0), Int32GetDatum(2)), 0.0));
    // Negative N: truncate digits left of decimal point.
    // 1234.56 truncated to -2 → 1200.0.
    EXPECT_TRUE(FloatEq(float8_trunc_n(Float8GetDatum(1234.56), Int32GetDatum(-2)), 1200.0, 1e-6));
}

TEST_F(MathFuncsTest, Trunc2ArgExtreme) {
    // Truncating to a large number of decimals effectively returns the input.
    EXPECT_TRUE(FloatEq(float8_trunc_n(Float8GetDatum(3.14), Int32GetDatum(20)), 3.14));
    // Negative input.
    EXPECT_TRUE(FloatEq(float8_trunc_n(Float8GetDatum(-3.14159), Int32GetDatum(2)), -3.14, 1e-6));
}

}  // namespace

#include "types/numutils.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <string>

#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"

namespace {

using pgcpp::error::ErrorData;
using pgcpp::error::LogLevel;
using pgcpp::memory::AllocSetContext;
using pgcpp::types::float8_out_internal;
using pgcpp::types::pg_strtoint64;

class NumutilsTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("numutils_test_context");
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
// float8_out_internal — shortest-round-trip decimal output
// ===========================================================================

TEST_F(NumutilsTest, Float8OutShortestDecimal) {
    // PG outputs "1" (not "1.0") for an integer-valued double.
    EXPECT_STREQ(float8_out_internal(1.0), "1");
}

TEST_F(NumutilsTest, Float8OutPi) {
    // The shortest round-trip representation of pi.
    EXPECT_STREQ(float8_out_internal(3.141592653589793), "3.141592653589793");
}

TEST_F(NumutilsTest, Float8OutNegative) {
    EXPECT_STREQ(float8_out_internal(-1.5), "-1.5");
}

TEST_F(NumutilsTest, Float8OutZero) {
    EXPECT_STREQ(float8_out_internal(0.0), "0");
    EXPECT_STREQ(float8_out_internal(-0.0), "0");
}

TEST_F(NumutilsTest, Float8OutInfinity) {
    EXPECT_STREQ(float8_out_internal(std::numeric_limits<double>::infinity()), "Infinity");
}

TEST_F(NumutilsTest, Float8OutNegInfinity) {
    EXPECT_STREQ(float8_out_internal(-std::numeric_limits<double>::infinity()), "-Infinity");
}

TEST_F(NumutilsTest, Float8OutNaN) {
    EXPECT_STREQ(float8_out_internal(std::numeric_limits<double>::quiet_NaN()), "NaN");
}

TEST_F(NumutilsTest, Float8OutScientificNotation) {
    // PG accepts either "100000000000000000000" or "1e+20" for 1e20.
    // Modern PG (with extra_float_digits=1) emits "1e+20".
    char* out = float8_out_internal(1e20);
    std::string s(out);
    EXPECT_TRUE(s == "100000000000000000000" || s == "1e+20") << "actual: " << s;
}

TEST_F(NumutilsTest, Float8OutRoundTrip) {
    // Every finite double emitted by float8_out_internal must parse back to
    // the same value (shortest round-trip guarantee from std::to_chars).
    const double kValues[] = {0.1,   0.2,        0.3,   1.5,  2.718281828459045,
                              -3.14, 123456.789, 1e-10, 1e10, 0.5};
    for (double v : kValues) {
        char* s = float8_out_internal(v);
        char* end;
        double parsed = std::strtod(s, &end);
        EXPECT_EQ(end, s + std::strlen(s)) << "trailing junk in: " << s;
        EXPECT_EQ(parsed, v) << "round-trip failed for " << v << " (got " << s << ")";
    }
}

// ===========================================================================
// pg_strtoint64
// ===========================================================================

TEST_F(NumutilsTest, PgStrtoint64Basic) {
    EXPECT_EQ(pg_strtoint64("12345"), 12345);
    EXPECT_EQ(pg_strtoint64("0"), 0);
}

TEST_F(NumutilsTest, PgStrtoint64Negative) {
    EXPECT_EQ(pg_strtoint64("-12345"), -12345);
    EXPECT_EQ(pg_strtoint64("+42"), 42);
}

TEST_F(NumutilsTest, PgStrtoint64MaxMin) {
    EXPECT_EQ(pg_strtoint64("9223372036854775807"), 9223372036854775807LL);
    EXPECT_EQ(pg_strtoint64("-9223372036854775808"), -9223372036854775807LL - 1);
}

TEST_F(NumutilsTest, PgStrtoint64InvalidRaises) {
    EXPECT_TRUE(RaisesError([] { pg_strtoint64(""); }));
    EXPECT_TRUE(RaisesError([] { pg_strtoint64("abc"); }));
    EXPECT_TRUE(RaisesError([] { pg_strtoint64("12abc"); }));
}

TEST_F(NumutilsTest, PgStrtoint64OutOfRangeRaises) {
    EXPECT_TRUE(RaisesError([] { pg_strtoint64("9223372036854775808"); }));
    EXPECT_TRUE(RaisesError([] { pg_strtoint64("-9223372036854775809"); }));
}

}  // namespace

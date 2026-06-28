#include "pgcpp/types/builtins.hpp"

#include <gtest/gtest.h>

#include <string>

#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/alloc_set.hpp"
#include "pgcpp/common/memory/memory_context.hpp"
#include "pgcpp/types/datetime.hpp"

namespace {

using pgcpp::error::ErrorData;
using pgcpp::error::LogLevel;
using pgcpp::memory::AllocSetContext;
using pgcpp::types::BoolGetDatum;
using pgcpp::types::DatumGetBool;
using pgcpp::types::DatumGetFloat8;
using pgcpp::types::DatumGetInt16;
using pgcpp::types::DatumGetInt32;
using pgcpp::types::DatumGetInt64;
using pgcpp::types::Float8GetDatum;
using pgcpp::types::Int16GetDatum;
using pgcpp::types::Int32GetDatum;
using pgcpp::types::Int64GetDatum;
using pgcpp::types::MakeTextDatum;
using pgcpp::types::TextDatumToString;

class BuiltinsTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("test_context");
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

// ---------------------------------------------------------------------------
// bool
// ---------------------------------------------------------------------------

TEST_F(BuiltinsTest, BoolInTrueVariants) {
    EXPECT_TRUE(DatumGetBool(pgcpp::types::bool_in("true")));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::bool_in("t")));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::bool_in("yes")));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::bool_in("y")));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::bool_in("on")));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::bool_in("1")));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::bool_in("TRUE")));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::bool_in("Yes")));
}

TEST_F(BuiltinsTest, BoolInFalseVariants) {
    EXPECT_FALSE(DatumGetBool(pgcpp::types::bool_in("false")));
    EXPECT_FALSE(DatumGetBool(pgcpp::types::bool_in("f")));
    EXPECT_FALSE(DatumGetBool(pgcpp::types::bool_in("no")));
    EXPECT_FALSE(DatumGetBool(pgcpp::types::bool_in("n")));
    EXPECT_FALSE(DatumGetBool(pgcpp::types::bool_in("off")));
    EXPECT_FALSE(DatumGetBool(pgcpp::types::bool_in("0")));
    EXPECT_FALSE(DatumGetBool(pgcpp::types::bool_in("FALSE")));
}

TEST_F(BuiltinsTest, BoolInInvalidRaises) {
    EXPECT_TRUE(RaisesError([] { pgcpp::types::bool_in("maybe"); }));
    EXPECT_TRUE(RaisesError([] { pgcpp::types::bool_in("2"); }));
    EXPECT_TRUE(RaisesError([] { pgcpp::types::bool_in(""); }));
}

TEST_F(BuiltinsTest, BoolOut) {
    EXPECT_STREQ(pgcpp::types::bool_out(BoolGetDatum(true)), "t");
    EXPECT_STREQ(pgcpp::types::bool_out(BoolGetDatum(false)), "f");
}

// ---------------------------------------------------------------------------
// int4
// ---------------------------------------------------------------------------

TEST_F(BuiltinsTest, Int4In) {
    EXPECT_EQ(DatumGetInt32(pgcpp::types::int4_in("42")), 42);
    EXPECT_EQ(DatumGetInt32(pgcpp::types::int4_in("-7")), -7);
    EXPECT_EQ(DatumGetInt32(pgcpp::types::int4_in("0")), 0);
}

TEST_F(BuiltinsTest, Int4InInvalidRaises) {
    EXPECT_TRUE(RaisesError([] { pgcpp::types::int4_in("invalid"); }));
    EXPECT_TRUE(RaisesError([] { pgcpp::types::int4_in(""); }));
    EXPECT_TRUE(RaisesError([] { pgcpp::types::int4_in("12abc"); }));
}

TEST_F(BuiltinsTest, Int4Out) {
    EXPECT_STREQ(pgcpp::types::int4_out(Int32GetDatum(42)), "42");
    EXPECT_STREQ(pgcpp::types::int4_out(Int32GetDatum(-7)), "-7");
    EXPECT_STREQ(pgcpp::types::int4_out(Int32GetDatum(0)), "0");
}

// ---------------------------------------------------------------------------
// int8
// ---------------------------------------------------------------------------

TEST_F(BuiltinsTest, Int8RoundTrip) {
    const char* kMax = "9223372036854775807";
    auto d = pgcpp::types::int8_in(kMax);
    EXPECT_EQ(DatumGetInt64(d), 9223372036854775807LL);
    EXPECT_STREQ(pgcpp::types::int8_out(d), kMax);

    const char* kMin = "-9223372036854775808";
    auto dmin = pgcpp::types::int8_in(kMin);
    EXPECT_EQ(DatumGetInt64(dmin), -9223372036854775807LL - 1);
    EXPECT_STREQ(pgcpp::types::int8_out(dmin), kMin);
}

TEST_F(BuiltinsTest, Int8InInvalidRaises) {
    EXPECT_TRUE(RaisesError([] { pgcpp::types::int8_in("not_a_number"); }));
}

// ---------------------------------------------------------------------------
// float8
// ---------------------------------------------------------------------------

TEST_F(BuiltinsTest, Float8RoundTrip) {
    auto d = pgcpp::types::float8_in("3.14");
    EXPECT_NEAR(DatumGetFloat8(d), 3.14, 1e-12);
    EXPECT_STREQ(pgcpp::types::float8_out(d), "3.14");
}

TEST_F(BuiltinsTest, Float8InInvalidRaises) {
    EXPECT_TRUE(RaisesError([] { pgcpp::types::float8_in("not_a_float"); }));
    EXPECT_TRUE(RaisesError([] { pgcpp::types::float8_in(""); }));
}

TEST_F(BuiltinsTest, Float8OutIntegerValue) {
    auto d = pgcpp::types::float8_in("7");
    EXPECT_STREQ(pgcpp::types::float8_out(d), "7");
}

// ---------------------------------------------------------------------------
// text / varchar
// ---------------------------------------------------------------------------

TEST_F(BuiltinsTest, TextRoundTrip) {
    auto d = pgcpp::types::text_in("hello world");
    EXPECT_STREQ(pgcpp::types::text_out(d), "hello world");
}

TEST_F(BuiltinsTest, TextEmpty) {
    auto d = pgcpp::types::text_in("");
    EXPECT_STREQ(pgcpp::types::text_out(d), "");
}

TEST_F(BuiltinsTest, VarcharRoundTrip) {
    auto d = pgcpp::types::varchar_in("hello world");
    EXPECT_STREQ(pgcpp::types::varchar_out(d), "hello world");
}

// ---------------------------------------------------------------------------
// Comparison functions
// ---------------------------------------------------------------------------

TEST_F(BuiltinsTest, Int4Cmp) {
    EXPECT_EQ(pgcpp::types::int4_cmp(Int32GetDatum(1), Int32GetDatum(2)), -1);
    EXPECT_EQ(pgcpp::types::int4_cmp(Int32GetDatum(2), Int32GetDatum(2)), 0);
    EXPECT_EQ(pgcpp::types::int4_cmp(Int32GetDatum(3), Int32GetDatum(2)), 1);
}

TEST_F(BuiltinsTest, Int8Cmp) {
    EXPECT_EQ(pgcpp::types::int8_cmp(Int64GetDatum(1), Int64GetDatum(2)), -1);
    EXPECT_EQ(pgcpp::types::int8_cmp(Int64GetDatum(2), Int64GetDatum(2)), 0);
    EXPECT_EQ(pgcpp::types::int8_cmp(Int64GetDatum(3), Int64GetDatum(2)), 1);
}

TEST_F(BuiltinsTest, Float8Cmp) {
    EXPECT_EQ(pgcpp::types::float8_cmp(Float8GetDatum(1.0), Float8GetDatum(2.0)), -1);
    EXPECT_EQ(pgcpp::types::float8_cmp(Float8GetDatum(2.0), Float8GetDatum(2.0)), 0);
    EXPECT_EQ(pgcpp::types::float8_cmp(Float8GetDatum(3.0), Float8GetDatum(2.0)), 1);
}

TEST_F(BuiltinsTest, TextCmp) {
    auto a = pgcpp::types::text_in("abc");
    auto b = pgcpp::types::text_in("abd");
    auto c = pgcpp::types::text_in("abc");
    EXPECT_EQ(pgcpp::types::text_cmp(a, b), -1);
    EXPECT_EQ(pgcpp::types::text_cmp(a, c), 0);
    EXPECT_EQ(pgcpp::types::text_cmp(b, a), 1);
}

TEST_F(BuiltinsTest, TextCmpDifferentLength) {
    auto short_t = pgcpp::types::text_in("ab");
    auto long_t = pgcpp::types::text_in("abc");
    EXPECT_EQ(pgcpp::types::text_cmp(short_t, long_t), -1);
    EXPECT_EQ(pgcpp::types::text_cmp(long_t, short_t), 1);
}

// ---------------------------------------------------------------------------
// Arithmetic — int4
// ---------------------------------------------------------------------------

TEST_F(BuiltinsTest, Int4Arithmetic) {
    EXPECT_EQ(DatumGetInt32(pgcpp::types::int4_pl(Int32GetDatum(3), Int32GetDatum(4))), 7);
    EXPECT_EQ(DatumGetInt32(pgcpp::types::int4_mi(Int32GetDatum(10), Int32GetDatum(3))), 7);
    EXPECT_EQ(DatumGetInt32(pgcpp::types::int4_mul(Int32GetDatum(3), Int32GetDatum(4))), 12);
    EXPECT_EQ(DatumGetInt32(pgcpp::types::int4_div(Int32GetDatum(12), Int32GetDatum(4))), 3);
}

TEST_F(BuiltinsTest, Int4DivByZeroRaises) {
    EXPECT_TRUE(RaisesError([] { pgcpp::types::int4_div(Int32GetDatum(10), Int32GetDatum(0)); }));
}

// ---------------------------------------------------------------------------
// Arithmetic — float8
// ---------------------------------------------------------------------------

TEST_F(BuiltinsTest, Float8Arithmetic) {
    EXPECT_NEAR(DatumGetFloat8(pgcpp::types::float8_pl(Float8GetDatum(3.0), Float8GetDatum(4.0))),
                7.0, 1e-12);
    EXPECT_NEAR(DatumGetFloat8(pgcpp::types::float8_mi(Float8GetDatum(10.0), Float8GetDatum(3.0))),
                7.0, 1e-12);
    EXPECT_NEAR(DatumGetFloat8(pgcpp::types::float8_mul(Float8GetDatum(3.0), Float8GetDatum(4.0))),
                12.0, 1e-12);
    EXPECT_NEAR(DatumGetFloat8(pgcpp::types::float8_div(Float8GetDatum(12.0), Float8GetDatum(4.0))),
                3.0, 1e-12);
}

TEST_F(BuiltinsTest, Float8DivByZeroRaises) {
    EXPECT_TRUE(
        RaisesError([] { pgcpp::types::float8_div(Float8GetDatum(10.0), Float8GetDatum(0.0)); }));
}

// ---------------------------------------------------------------------------
// text concatenation
// ---------------------------------------------------------------------------

TEST_F(BuiltinsTest, TextConcat) {
    auto a = pgcpp::types::text_in("hello ");
    auto b = pgcpp::types::text_in("world");
    auto result = pgcpp::types::text_concat(a, b);
    EXPECT_STREQ(pgcpp::types::text_out(result), "hello world");
}

TEST_F(BuiltinsTest, TextConcatWithEmpty) {
    auto a = pgcpp::types::text_in("abc");
    auto empty = pgcpp::types::text_in("");
    auto result = pgcpp::types::text_concat(a, empty);
    EXPECT_STREQ(pgcpp::types::text_out(result), "abc");
}

// ---------------------------------------------------------------------------
// Helpers: MakeTextDatum / TextDatumToString
// ---------------------------------------------------------------------------

TEST_F(BuiltinsTest, MakeTextDatumRoundTrip) {
    auto d = MakeTextDatum("hello world");
    EXPECT_EQ(TextDatumToString(d), "hello world");
}

TEST_F(BuiltinsTest, MakeTextDatumEmpty) {
    auto d = MakeTextDatum("");
    EXPECT_EQ(TextDatumToString(d), "");
}

TEST_F(BuiltinsTest, MakeTextDatumInteropWithTextOut) {
    auto d = MakeTextDatum("interop");
    EXPECT_STREQ(pgcpp::types::text_out(d), "interop");
}

TEST_F(BuiltinsTest, TextInInteropWithTextDatumToString) {
    auto d = pgcpp::types::text_in("from text_in");
    EXPECT_EQ(TextDatumToString(d), "from text_in");
}

// ---------------------------------------------------------------------------
// int2 (SMALLINT)
// ---------------------------------------------------------------------------

TEST_F(BuiltinsTest, Int2InOutRoundTrip) {
    auto d = pgcpp::types::int2_in("42");
    EXPECT_EQ(DatumGetInt16(d), 42);
    EXPECT_STREQ(pgcpp::types::int2_out(d), "42");
    auto neg = pgcpp::types::int2_in("-7");
    EXPECT_EQ(DatumGetInt16(neg), -7);
    EXPECT_STREQ(pgcpp::types::int2_out(neg), "-7");
}

TEST_F(BuiltinsTest, Int2OutOfRangeRaises) {
    EXPECT_TRUE(RaisesError([] { pgcpp::types::int2_in("40000"); }));
    EXPECT_TRUE(RaisesError([] { pgcpp::types::int2_in("-40000"); }));
}

TEST_F(BuiltinsTest, Int2Eq) {
    EXPECT_TRUE(DatumGetBool(pgcpp::types::int2_eq(Int16GetDatum(5), Int16GetDatum(5))));
    EXPECT_FALSE(DatumGetBool(pgcpp::types::int2_eq(Int16GetDatum(5), Int16GetDatum(6))));
}

TEST_F(BuiltinsTest, Int2Lt) {
    EXPECT_TRUE(DatumGetBool(pgcpp::types::int2_lt(Int16GetDatum(5), Int16GetDatum(6))));
    EXPECT_FALSE(DatumGetBool(pgcpp::types::int2_lt(Int16GetDatum(6), Int16GetDatum(5))));
    EXPECT_FALSE(DatumGetBool(pgcpp::types::int2_lt(Int16GetDatum(5), Int16GetDatum(5))));
}

TEST_F(BuiltinsTest, Int2Pl) {
    EXPECT_EQ(DatumGetInt16(pgcpp::types::int2_pl(Int16GetDatum(3), Int16GetDatum(4))), 7);
}

TEST_F(BuiltinsTest, Int2Div) {
    EXPECT_EQ(DatumGetInt16(pgcpp::types::int2_div(Int16GetDatum(12), Int16GetDatum(4))), 3);
    EXPECT_TRUE(RaisesError([] { pgcpp::types::int2_div(Int16GetDatum(10), Int16GetDatum(0)); }));
}

// ---------------------------------------------------------------------------
// int4 comparison + extras
// ---------------------------------------------------------------------------

TEST_F(BuiltinsTest, Int4Eq) {
    EXPECT_TRUE(DatumGetBool(pgcpp::types::int4_eq(Int32GetDatum(5), Int32GetDatum(5))));
    EXPECT_FALSE(DatumGetBool(pgcpp::types::int4_ne(Int32GetDatum(5), Int32GetDatum(5))));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::int4_lt(Int32GetDatum(5), Int32GetDatum(6))));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::int4_ge(Int32GetDatum(6), Int32GetDatum(6))));
}

TEST_F(BuiltinsTest, Int4Mod) {
    EXPECT_EQ(DatumGetInt32(pgcpp::types::int4_mod(Int32GetDatum(17), Int32GetDatum(5))), 2);
    EXPECT_TRUE(RaisesError([] { pgcpp::types::int4_mod(Int32GetDatum(10), Int32GetDatum(0)); }));
}

TEST_F(BuiltinsTest, Int4AbsAndUmAndInc) {
    EXPECT_EQ(DatumGetInt32(pgcpp::types::int4_abs(Int32GetDatum(-7))), 7);
    EXPECT_EQ(DatumGetInt32(pgcpp::types::int4_abs(Int32GetDatum(7))), 7);
    EXPECT_EQ(DatumGetInt32(pgcpp::types::int4_um(Int32GetDatum(7))), -7);
    EXPECT_EQ(DatumGetInt32(pgcpp::types::int4_inc(Int32GetDatum(41))), 42);
}

// ---------------------------------------------------------------------------
// int8 comparison + extras
// ---------------------------------------------------------------------------

TEST_F(BuiltinsTest, Int8Eq) {
    EXPECT_TRUE(DatumGetBool(pgcpp::types::int8_eq(Int64GetDatum(100), Int64GetDatum(100))));
    EXPECT_FALSE(DatumGetBool(pgcpp::types::int8_ne(Int64GetDatum(100), Int64GetDatum(100))));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::int8_lt(Int64GetDatum(99), Int64GetDatum(100))));
}

TEST_F(BuiltinsTest, Int8Pl) {
    EXPECT_EQ(DatumGetInt64(pgcpp::types::int8_pl(Int64GetDatum(1000000000), Int64GetDatum(23))),
              1000000023);
}

TEST_F(BuiltinsTest, Int8Mod) {
    EXPECT_EQ(DatumGetInt64(pgcpp::types::int8_mod(Int64GetDatum(17), Int64GetDatum(5))), 2);
    EXPECT_TRUE(RaisesError([] { pgcpp::types::int8_mod(Int64GetDatum(10), Int64GetDatum(0)); }));
}

TEST_F(BuiltinsTest, Int8AbsAndUmAndInc) {
    EXPECT_EQ(DatumGetInt64(pgcpp::types::int8_abs(Int64GetDatum(-9))), 9);
    EXPECT_EQ(DatumGetInt64(pgcpp::types::int8_um(Int64GetDatum(9))), -9);
    EXPECT_EQ(DatumGetInt64(pgcpp::types::int8_inc(Int64GetDatum(41))), 42);
}

// ---------------------------------------------------------------------------
// float8 comparison + math functions
// ---------------------------------------------------------------------------

TEST_F(BuiltinsTest, Float8Eq) {
    EXPECT_TRUE(DatumGetBool(pgcpp::types::float8_eq(Float8GetDatum(1.5), Float8GetDatum(1.5))));
    EXPECT_FALSE(DatumGetBool(pgcpp::types::float8_ne(Float8GetDatum(1.5), Float8GetDatum(1.5))));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::float8_lt(Float8GetDatum(1.4), Float8GetDatum(1.5))));
}

TEST_F(BuiltinsTest, Float8Ceil) {
    EXPECT_NEAR(DatumGetFloat8(pgcpp::types::float8_ceil(Float8GetDatum(1.4))), 2.0, 1e-12);
    EXPECT_NEAR(DatumGetFloat8(pgcpp::types::float8_ceil(Float8GetDatum(-1.4))), -1.0, 1e-12);
}

TEST_F(BuiltinsTest, Float8Floor) {
    EXPECT_NEAR(DatumGetFloat8(pgcpp::types::float8_floor(Float8GetDatum(1.6))), 1.0, 1e-12);
    EXPECT_NEAR(DatumGetFloat8(pgcpp::types::float8_floor(Float8GetDatum(-1.6))), -2.0, 1e-12);
}

TEST_F(BuiltinsTest, Float8AbsAndUm) {
    EXPECT_NEAR(DatumGetFloat8(pgcpp::types::float8_abs(Float8GetDatum(-3.5))), 3.5, 1e-12);
    EXPECT_NEAR(DatumGetFloat8(pgcpp::types::float8_um(Float8GetDatum(3.5))), -3.5, 1e-12);
}

// ---------------------------------------------------------------------------
// bool operators
// ---------------------------------------------------------------------------

TEST_F(BuiltinsTest, BoolOperators) {
    EXPECT_TRUE(DatumGetBool(pgcpp::types::bool_eq(BoolGetDatum(true), BoolGetDatum(true))));
    EXPECT_FALSE(DatumGetBool(pgcpp::types::bool_eq(BoolGetDatum(true), BoolGetDatum(false))));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::bool_ne(BoolGetDatum(true), BoolGetDatum(false))));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::bool_lt(BoolGetDatum(false), BoolGetDatum(true))));
}

// ---------------------------------------------------------------------------
// text comparison operators
// ---------------------------------------------------------------------------

TEST_F(BuiltinsTest, TextEq) {
    auto a = pgcpp::types::text_in("abc");
    auto b = pgcpp::types::text_in("abc");
    auto c = pgcpp::types::text_in("abd");
    EXPECT_TRUE(DatumGetBool(pgcpp::types::text_eq(a, b)));
    EXPECT_FALSE(DatumGetBool(pgcpp::types::text_eq(a, c)));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::text_ne(a, c)));
}

TEST_F(BuiltinsTest, TextLt) {
    auto a = pgcpp::types::text_in("abc");
    auto b = pgcpp::types::text_in("abd");
    EXPECT_TRUE(DatumGetBool(pgcpp::types::text_lt(a, b)));
    EXPECT_FALSE(DatumGetBool(pgcpp::types::text_lt(b, a)));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::text_le(a, b)));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::text_ge(b, a)));
}

// ---------------------------------------------------------------------------
// timestamp/date comparison operators
// ---------------------------------------------------------------------------

TEST_F(BuiltinsTest, TimestampEq) {
    auto a = pgcpp::types::timestamp_in("2013-07-15 10:30:45");
    auto b = pgcpp::types::timestamp_in("2013-07-15 10:30:45");
    auto c = pgcpp::types::timestamp_in("2013-07-15 10:30:46");
    EXPECT_TRUE(DatumGetBool(pgcpp::types::timestamp_eq(a, b)));
    EXPECT_FALSE(DatumGetBool(pgcpp::types::timestamp_eq(a, c)));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::timestamp_lt(a, c)));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::timestamp_ge(a, b)));
}

TEST_F(BuiltinsTest, DateLt) {
    auto a = pgcpp::types::date_in("2013-07-15");
    auto b = pgcpp::types::date_in("2013-07-16");
    EXPECT_TRUE(DatumGetBool(pgcpp::types::date_lt(a, b)));
    EXPECT_FALSE(DatumGetBool(pgcpp::types::date_lt(b, a)));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::date_eq(a, a)));
    EXPECT_TRUE(DatumGetBool(pgcpp::types::date_le(a, b)));
}

// ---------------------------------------------------------------------------
// type conversions
// ---------------------------------------------------------------------------

TEST_F(BuiltinsTest, I2ToI4AndBack) {
    EXPECT_EQ(DatumGetInt32(pgcpp::types::i2toi4(Int16GetDatum(-42))), -42);
    EXPECT_EQ(DatumGetInt16(pgcpp::types::i4toi2(Int32GetDatum(-42))), -42);
}

TEST_F(BuiltinsTest, I4ToI2OutOfRangeRaises) {
    EXPECT_TRUE(RaisesError([] { pgcpp::types::i4toi2(Int32GetDatum(40000)); }));
}

}  // namespace

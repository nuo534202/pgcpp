#include "mytoydb/types/builtins.hpp"

#include <gtest/gtest.h>

#include <string>

#include "mytoydb/common/error/elog.hpp"
#include "mytoydb/common/memory/alloc_set.hpp"
#include "mytoydb/common/memory/memory_context.hpp"

namespace {

using mytoydb::error::ErrorData;
using mytoydb::error::LogLevel;
using mytoydb::memory::AllocSetContext;
using mytoydb::types::BoolGetDatum;
using mytoydb::types::DatumGetBool;
using mytoydb::types::DatumGetFloat8;
using mytoydb::types::DatumGetInt32;
using mytoydb::types::DatumGetInt64;
using mytoydb::types::Float8GetDatum;
using mytoydb::types::Int32GetDatum;
using mytoydb::types::Int64GetDatum;
using mytoydb::types::MakeTextDatum;
using mytoydb::types::TextDatumToString;

class BuiltinsTest : public ::testing::Test {
protected:
    void SetUp() override {
        mytoydb::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("test_context");
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

// ---------------------------------------------------------------------------
// bool
// ---------------------------------------------------------------------------

TEST_F(BuiltinsTest, BoolInTrueVariants) {
    EXPECT_TRUE(DatumGetBool(mytoydb::types::bool_in("true")));
    EXPECT_TRUE(DatumGetBool(mytoydb::types::bool_in("t")));
    EXPECT_TRUE(DatumGetBool(mytoydb::types::bool_in("yes")));
    EXPECT_TRUE(DatumGetBool(mytoydb::types::bool_in("y")));
    EXPECT_TRUE(DatumGetBool(mytoydb::types::bool_in("on")));
    EXPECT_TRUE(DatumGetBool(mytoydb::types::bool_in("1")));
    EXPECT_TRUE(DatumGetBool(mytoydb::types::bool_in("TRUE")));
    EXPECT_TRUE(DatumGetBool(mytoydb::types::bool_in("Yes")));
}

TEST_F(BuiltinsTest, BoolInFalseVariants) {
    EXPECT_FALSE(DatumGetBool(mytoydb::types::bool_in("false")));
    EXPECT_FALSE(DatumGetBool(mytoydb::types::bool_in("f")));
    EXPECT_FALSE(DatumGetBool(mytoydb::types::bool_in("no")));
    EXPECT_FALSE(DatumGetBool(mytoydb::types::bool_in("n")));
    EXPECT_FALSE(DatumGetBool(mytoydb::types::bool_in("off")));
    EXPECT_FALSE(DatumGetBool(mytoydb::types::bool_in("0")));
    EXPECT_FALSE(DatumGetBool(mytoydb::types::bool_in("FALSE")));
}

TEST_F(BuiltinsTest, BoolInInvalidRaises) {
    EXPECT_TRUE(RaisesError([] { mytoydb::types::bool_in("maybe"); }));
    EXPECT_TRUE(RaisesError([] { mytoydb::types::bool_in("2"); }));
    EXPECT_TRUE(RaisesError([] { mytoydb::types::bool_in(""); }));
}

TEST_F(BuiltinsTest, BoolOut) {
    EXPECT_STREQ(mytoydb::types::bool_out(BoolGetDatum(true)), "t");
    EXPECT_STREQ(mytoydb::types::bool_out(BoolGetDatum(false)), "f");
}

// ---------------------------------------------------------------------------
// int4
// ---------------------------------------------------------------------------

TEST_F(BuiltinsTest, Int4In) {
    EXPECT_EQ(DatumGetInt32(mytoydb::types::int4_in("42")), 42);
    EXPECT_EQ(DatumGetInt32(mytoydb::types::int4_in("-7")), -7);
    EXPECT_EQ(DatumGetInt32(mytoydb::types::int4_in("0")), 0);
}

TEST_F(BuiltinsTest, Int4InInvalidRaises) {
    EXPECT_TRUE(RaisesError([] { mytoydb::types::int4_in("invalid"); }));
    EXPECT_TRUE(RaisesError([] { mytoydb::types::int4_in(""); }));
    EXPECT_TRUE(RaisesError([] { mytoydb::types::int4_in("12abc"); }));
}

TEST_F(BuiltinsTest, Int4Out) {
    EXPECT_STREQ(mytoydb::types::int4_out(Int32GetDatum(42)), "42");
    EXPECT_STREQ(mytoydb::types::int4_out(Int32GetDatum(-7)), "-7");
    EXPECT_STREQ(mytoydb::types::int4_out(Int32GetDatum(0)), "0");
}

// ---------------------------------------------------------------------------
// int8
// ---------------------------------------------------------------------------

TEST_F(BuiltinsTest, Int8RoundTrip) {
    const char* kMax = "9223372036854775807";
    auto d = mytoydb::types::int8_in(kMax);
    EXPECT_EQ(DatumGetInt64(d), 9223372036854775807LL);
    EXPECT_STREQ(mytoydb::types::int8_out(d), kMax);

    const char* kMin = "-9223372036854775808";
    auto dmin = mytoydb::types::int8_in(kMin);
    EXPECT_EQ(DatumGetInt64(dmin), -9223372036854775807LL - 1);
    EXPECT_STREQ(mytoydb::types::int8_out(dmin), kMin);
}

TEST_F(BuiltinsTest, Int8InInvalidRaises) {
    EXPECT_TRUE(RaisesError([] { mytoydb::types::int8_in("not_a_number"); }));
}

// ---------------------------------------------------------------------------
// float8
// ---------------------------------------------------------------------------

TEST_F(BuiltinsTest, Float8RoundTrip) {
    auto d = mytoydb::types::float8_in("3.14");
    EXPECT_NEAR(DatumGetFloat8(d), 3.14, 1e-12);
    EXPECT_STREQ(mytoydb::types::float8_out(d), "3.14");
}

TEST_F(BuiltinsTest, Float8InInvalidRaises) {
    EXPECT_TRUE(RaisesError([] { mytoydb::types::float8_in("not_a_float"); }));
    EXPECT_TRUE(RaisesError([] { mytoydb::types::float8_in(""); }));
}

TEST_F(BuiltinsTest, Float8OutIntegerValue) {
    auto d = mytoydb::types::float8_in("7");
    EXPECT_STREQ(mytoydb::types::float8_out(d), "7");
}

// ---------------------------------------------------------------------------
// text / varchar
// ---------------------------------------------------------------------------

TEST_F(BuiltinsTest, TextRoundTrip) {
    auto d = mytoydb::types::text_in("hello world");
    EXPECT_STREQ(mytoydb::types::text_out(d), "hello world");
}

TEST_F(BuiltinsTest, TextEmpty) {
    auto d = mytoydb::types::text_in("");
    EXPECT_STREQ(mytoydb::types::text_out(d), "");
}

TEST_F(BuiltinsTest, VarcharRoundTrip) {
    auto d = mytoydb::types::varchar_in("hello world");
    EXPECT_STREQ(mytoydb::types::varchar_out(d), "hello world");
}

// ---------------------------------------------------------------------------
// Comparison functions
// ---------------------------------------------------------------------------

TEST_F(BuiltinsTest, Int4Cmp) {
    EXPECT_EQ(mytoydb::types::int4_cmp(Int32GetDatum(1), Int32GetDatum(2)), -1);
    EXPECT_EQ(mytoydb::types::int4_cmp(Int32GetDatum(2), Int32GetDatum(2)), 0);
    EXPECT_EQ(mytoydb::types::int4_cmp(Int32GetDatum(3), Int32GetDatum(2)), 1);
}

TEST_F(BuiltinsTest, Int8Cmp) {
    EXPECT_EQ(mytoydb::types::int8_cmp(Int64GetDatum(1), Int64GetDatum(2)), -1);
    EXPECT_EQ(mytoydb::types::int8_cmp(Int64GetDatum(2), Int64GetDatum(2)), 0);
    EXPECT_EQ(mytoydb::types::int8_cmp(Int64GetDatum(3), Int64GetDatum(2)), 1);
}

TEST_F(BuiltinsTest, Float8Cmp) {
    EXPECT_EQ(mytoydb::types::float8_cmp(Float8GetDatum(1.0), Float8GetDatum(2.0)), -1);
    EXPECT_EQ(mytoydb::types::float8_cmp(Float8GetDatum(2.0), Float8GetDatum(2.0)), 0);
    EXPECT_EQ(mytoydb::types::float8_cmp(Float8GetDatum(3.0), Float8GetDatum(2.0)), 1);
}

TEST_F(BuiltinsTest, TextCmp) {
    auto a = mytoydb::types::text_in("abc");
    auto b = mytoydb::types::text_in("abd");
    auto c = mytoydb::types::text_in("abc");
    EXPECT_EQ(mytoydb::types::text_cmp(a, b), -1);
    EXPECT_EQ(mytoydb::types::text_cmp(a, c), 0);
    EXPECT_EQ(mytoydb::types::text_cmp(b, a), 1);
}

TEST_F(BuiltinsTest, TextCmpDifferentLength) {
    auto short_t = mytoydb::types::text_in("ab");
    auto long_t = mytoydb::types::text_in("abc");
    EXPECT_EQ(mytoydb::types::text_cmp(short_t, long_t), -1);
    EXPECT_EQ(mytoydb::types::text_cmp(long_t, short_t), 1);
}

// ---------------------------------------------------------------------------
// Arithmetic — int4
// ---------------------------------------------------------------------------

TEST_F(BuiltinsTest, Int4Arithmetic) {
    EXPECT_EQ(DatumGetInt32(mytoydb::types::int4_pl(Int32GetDatum(3), Int32GetDatum(4))), 7);
    EXPECT_EQ(DatumGetInt32(mytoydb::types::int4_mi(Int32GetDatum(10), Int32GetDatum(3))), 7);
    EXPECT_EQ(DatumGetInt32(mytoydb::types::int4_mul(Int32GetDatum(3), Int32GetDatum(4))), 12);
    EXPECT_EQ(DatumGetInt32(mytoydb::types::int4_div(Int32GetDatum(12), Int32GetDatum(4))), 3);
}

TEST_F(BuiltinsTest, Int4DivByZeroRaises) {
    EXPECT_TRUE(RaisesError([] { mytoydb::types::int4_div(Int32GetDatum(10), Int32GetDatum(0)); }));
}

// ---------------------------------------------------------------------------
// Arithmetic — float8
// ---------------------------------------------------------------------------

TEST_F(BuiltinsTest, Float8Arithmetic) {
    EXPECT_NEAR(DatumGetFloat8(mytoydb::types::float8_pl(Float8GetDatum(3.0), Float8GetDatum(4.0))),
                7.0, 1e-12);
    EXPECT_NEAR(
        DatumGetFloat8(mytoydb::types::float8_mi(Float8GetDatum(10.0), Float8GetDatum(3.0))), 7.0,
        1e-12);
    EXPECT_NEAR(
        DatumGetFloat8(mytoydb::types::float8_mul(Float8GetDatum(3.0), Float8GetDatum(4.0))), 12.0,
        1e-12);
    EXPECT_NEAR(
        DatumGetFloat8(mytoydb::types::float8_div(Float8GetDatum(12.0), Float8GetDatum(4.0))), 3.0,
        1e-12);
}

TEST_F(BuiltinsTest, Float8DivByZeroRaises) {
    EXPECT_TRUE(
        RaisesError([] { mytoydb::types::float8_div(Float8GetDatum(10.0), Float8GetDatum(0.0)); }));
}

// ---------------------------------------------------------------------------
// text concatenation
// ---------------------------------------------------------------------------

TEST_F(BuiltinsTest, TextConcat) {
    auto a = mytoydb::types::text_in("hello ");
    auto b = mytoydb::types::text_in("world");
    auto result = mytoydb::types::text_concat(a, b);
    EXPECT_STREQ(mytoydb::types::text_out(result), "hello world");
}

TEST_F(BuiltinsTest, TextConcatWithEmpty) {
    auto a = mytoydb::types::text_in("abc");
    auto empty = mytoydb::types::text_in("");
    auto result = mytoydb::types::text_concat(a, empty);
    EXPECT_STREQ(mytoydb::types::text_out(result), "abc");
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
    EXPECT_STREQ(mytoydb::types::text_out(d), "interop");
}

TEST_F(BuiltinsTest, TextInInteropWithTextDatumToString) {
    auto d = mytoydb::types::text_in("from text_in");
    EXPECT_EQ(TextDatumToString(d), "from text_in");
}

}  // namespace

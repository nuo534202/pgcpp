// string_funcs_ext_test.cpp — Unit tests for Task 10 string functions.
//
// Covers the Task 10 additions not already exercised by string_funcs_test.cpp:
//   - text_replace
//   - text_position
//   - text_lpad (2-arg and 3-arg)
//   - text_rpad (2-arg and 3-arg)
//   - text_split_part
//   - text_substr_2 (2-arg substr)
//   - text_trim (alias of text_btrim)
//
// Each function has at least three tests: a normal case, a boundary case
// (empty string, out-of-range index, etc.), and an error/edge case.

#include <gtest/gtest.h>

#include <string>

#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "types/builtins.hpp"
#include "types/datum.hpp"
#include "types/string_funcs.hpp"

namespace {

using pgcpp::error::ErrorData;
using pgcpp::error::LogLevel;
using pgcpp::memory::AllocSetContext;
using pgcpp::types::Datum;
using pgcpp::types::DatumGetInt32;
using pgcpp::types::Int32GetDatum;
using pgcpp::types::MakeTextDatum;
using pgcpp::types::text_btrim;
using pgcpp::types::text_lpad;
using pgcpp::types::text_position;
using pgcpp::types::text_replace;
using pgcpp::types::text_rpad;
using pgcpp::types::text_split_part;
using pgcpp::types::text_substr_2;
using pgcpp::types::text_trim;
using pgcpp::types::TextDatumToString;

class StringFuncsExtTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("string_funcs_ext_test_context");
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
// text_replace
// ===========================================================================

TEST_F(StringFuncsExtTest, ReplaceNormal) {
    EXPECT_EQ(TextDatumToString(text_replace(MakeTextDatum("hello world"), MakeTextDatum("o"),
                                             MakeTextDatum("0"))),
              "hell0 w0rld");
    EXPECT_EQ(TextDatumToString(
                  text_replace(MakeTextDatum("aaa"), MakeTextDatum("a"), MakeTextDatum("bb"))),
              "bbbbbb");
}

TEST_F(StringFuncsExtTest, ReplaceBoundary) {
    // Empty source.
    EXPECT_EQ(
        TextDatumToString(text_replace(MakeTextDatum(""), MakeTextDatum("a"), MakeTextDatum("b"))),
        "");
    // Replacement with empty string acts as deletion.
    EXPECT_EQ(TextDatumToString(
                  text_replace(MakeTextDatum("hello"), MakeTextDatum("l"), MakeTextDatum(""))),
              "heo");
}

TEST_F(StringFuncsExtTest, ReplaceEmptyFromIsNoOp) {
    // Empty `from` would loop forever; PostgreSQL returns the source.
    EXPECT_EQ(TextDatumToString(
                  text_replace(MakeTextDatum("hello"), MakeTextDatum(""), MakeTextDatum("X"))),
              "hello");
    EXPECT_EQ(TextDatumToString(
                  text_replace(MakeTextDatum("hello"), MakeTextDatum(""), MakeTextDatum(""))),
              "hello");
}

TEST_F(StringFuncsExtTest, ReplaceNotFound) {
    EXPECT_EQ(TextDatumToString(
                  text_replace(MakeTextDatum("hello"), MakeTextDatum("xyz"), MakeTextDatum("abc"))),
              "hello");
}

// ===========================================================================
// text_position
// ===========================================================================

TEST_F(StringFuncsExtTest, PositionNormal) {
    EXPECT_EQ(DatumGetInt32(text_position(MakeTextDatum("hello world"), MakeTextDatum("world"))),
              7);
    EXPECT_EQ(DatumGetInt32(text_position(MakeTextDatum("hello"), MakeTextDatum("l"))), 3);
}

TEST_F(StringFuncsExtTest, PositionNotFound) {
    EXPECT_EQ(DatumGetInt32(text_position(MakeTextDatum("hello"), MakeTextDatum("xyz"))), 0);
}

TEST_F(StringFuncsExtTest, PositionEmptySubstring) {
    // PostgreSQL: empty substring returns 1.
    EXPECT_EQ(DatumGetInt32(text_position(MakeTextDatum("hello"), MakeTextDatum(""))), 1);
    EXPECT_EQ(DatumGetInt32(text_position(MakeTextDatum(""), MakeTextDatum(""))), 1);
}

TEST_F(StringFuncsExtTest, PositionAtStart) {
    EXPECT_EQ(DatumGetInt32(text_position(MakeTextDatum("hello"), MakeTextDatum("hello"))), 1);
    EXPECT_EQ(DatumGetInt32(text_position(MakeTextDatum("hello"), MakeTextDatum("he"))), 1);
}

// ===========================================================================
// text_lpad (2-arg)
// ===========================================================================

TEST_F(StringFuncsExtTest, Lpad2ArgNormal) {
    // Pads with spaces to length 8.
    EXPECT_EQ(TextDatumToString(text_lpad(MakeTextDatum("hi"), Int32GetDatum(8))), "      hi");
}

TEST_F(StringFuncsExtTest, Lpad2ArgBoundary) {
    // Already at length.
    EXPECT_EQ(TextDatumToString(text_lpad(MakeTextDatum("hello"), Int32GetDatum(5))), "hello");
    // Length 0 returns empty string.
    EXPECT_EQ(TextDatumToString(text_lpad(MakeTextDatum("hi"), Int32GetDatum(0))), "");
}

TEST_F(StringFuncsExtTest, Lpad2ArgTruncates) {
    // If string is longer than target, truncate to target length.
    EXPECT_EQ(TextDatumToString(text_lpad(MakeTextDatum("hello"), Int32GetDatum(3))), "hel");
    // Negative length returns empty.
    EXPECT_EQ(TextDatumToString(text_lpad(MakeTextDatum("hi"), Int32GetDatum(-1))), "");
}

// ===========================================================================
// text_lpad (3-arg)
// ===========================================================================

TEST_F(StringFuncsExtTest, Lpad3ArgNormal) {
    EXPECT_EQ(
        TextDatumToString(text_lpad(MakeTextDatum("hi"), Int32GetDatum(8), MakeTextDatum("xy"))),
        "xyxyxyhi");
}

TEST_F(StringFuncsExtTest, Lpad3ArgBoundary) {
    EXPECT_EQ(
        TextDatumToString(text_lpad(MakeTextDatum("hi"), Int32GetDatum(2), MakeTextDatum("x"))),
        "hi");
    EXPECT_EQ(
        TextDatumToString(text_lpad(MakeTextDatum("hi"), Int32GetDatum(0), MakeTextDatum("x"))),
        "");
}

TEST_F(StringFuncsExtTest, Lpad3ArgEmptyFill) {
    // Empty fill: cannot pad, returns truncated input (target length).
    EXPECT_EQ(
        TextDatumToString(text_lpad(MakeTextDatum("hello"), Int32GetDatum(3), MakeTextDatum(""))),
        "hel");
}

// ===========================================================================
// text_rpad (2-arg)
// ===========================================================================

TEST_F(StringFuncsExtTest, Rpad2ArgNormal) {
    EXPECT_EQ(TextDatumToString(text_rpad(MakeTextDatum("hi"), Int32GetDatum(8))), "hi      ");
}

TEST_F(StringFuncsExtTest, Rpad2ArgBoundary) {
    EXPECT_EQ(TextDatumToString(text_rpad(MakeTextDatum("hello"), Int32GetDatum(5))), "hello");
    EXPECT_EQ(TextDatumToString(text_rpad(MakeTextDatum("hi"), Int32GetDatum(0))), "");
}

TEST_F(StringFuncsExtTest, Rpad2ArgTruncates) {
    EXPECT_EQ(TextDatumToString(text_rpad(MakeTextDatum("hello"), Int32GetDatum(3))), "hel");
}

// ===========================================================================
// text_rpad (3-arg)
// ===========================================================================

TEST_F(StringFuncsExtTest, Rpad3ArgNormal) {
    EXPECT_EQ(
        TextDatumToString(text_rpad(MakeTextDatum("hi"), Int32GetDatum(8), MakeTextDatum("xy"))),
        "hixyxyxy");
}

TEST_F(StringFuncsExtTest, Rpad3ArgBoundary) {
    EXPECT_EQ(
        TextDatumToString(text_rpad(MakeTextDatum("hi"), Int32GetDatum(2), MakeTextDatum("x"))),
        "hi");
    EXPECT_EQ(
        TextDatumToString(text_rpad(MakeTextDatum("hi"), Int32GetDatum(0), MakeTextDatum("x"))),
        "");
}

TEST_F(StringFuncsExtTest, Rpad3ArgEmptyFill) {
    EXPECT_EQ(
        TextDatumToString(text_rpad(MakeTextDatum("hello"), Int32GetDatum(3), MakeTextDatum(""))),
        "hel");
}

// ===========================================================================
// text_split_part
// ===========================================================================

TEST_F(StringFuncsExtTest, SplitPartNormal) {
    EXPECT_EQ(TextDatumToString(
                  text_split_part(MakeTextDatum("a-b-c"), MakeTextDatum("-"), Int32GetDatum(2))),
              "b");
    EXPECT_EQ(TextDatumToString(
                  text_split_part(MakeTextDatum("a-b-c"), MakeTextDatum("-"), Int32GetDatum(3))),
              "c");
}

TEST_F(StringFuncsExtTest, SplitPartBoundary) {
    // Field 1 returns the first piece.
    EXPECT_EQ(TextDatumToString(
                  text_split_part(MakeTextDatum("a-b-c"), MakeTextDatum("-"), Int32GetDatum(1))),
              "a");
    // Field out of range → empty string.
    EXPECT_EQ(TextDatumToString(
                  text_split_part(MakeTextDatum("a-b-c"), MakeTextDatum("-"), Int32GetDatum(10))),
              "");
    // Empty source → field 1 is "".
    EXPECT_EQ(
        TextDatumToString(text_split_part(MakeTextDatum(""), MakeTextDatum("-"), Int32GetDatum(1))),
        "");
}

TEST_F(StringFuncsExtTest, SplitPartNegativeField) {
    // Negative field counts from the end.
    EXPECT_EQ(TextDatumToString(
                  text_split_part(MakeTextDatum("a-b-c"), MakeTextDatum("-"), Int32GetDatum(-1))),
              "c");
    EXPECT_EQ(TextDatumToString(
                  text_split_part(MakeTextDatum("a-b-c"), MakeTextDatum("-"), Int32GetDatum(-3))),
              "a");
    // Out-of-range negative → empty.
    EXPECT_EQ(TextDatumToString(
                  text_split_part(MakeTextDatum("a-b-c"), MakeTextDatum("-"), Int32GetDatum(-10))),
              "");
}

TEST_F(StringFuncsExtTest, SplitPartErrorOnEmptySep) {
    EXPECT_TRUE(RaisesError(
        [] { text_split_part(MakeTextDatum("abc"), MakeTextDatum(""), Int32GetDatum(1)); }));
}

TEST_F(StringFuncsExtTest, SplitPartErrorOnZeroField) {
    EXPECT_TRUE(RaisesError(
        [] { text_split_part(MakeTextDatum("a-b-c"), MakeTextDatum("-"), Int32GetDatum(0)); }));
}

TEST_F(StringFuncsExtTest, SplitPartMultiCharSep) {
    // Multi-character separator.
    EXPECT_EQ(TextDatumToString(
                  text_split_part(MakeTextDatum("a__b__c"), MakeTextDatum("__"), Int32GetDatum(2))),
              "b");
}

// ===========================================================================
// text_substr_2 (2-arg substr)
// ===========================================================================

TEST_F(StringFuncsExtTest, Substr2Normal) {
    EXPECT_EQ(TextDatumToString(text_substr_2(MakeTextDatum("hello world"), Int32GetDatum(7))),
              "world");
    EXPECT_EQ(TextDatumToString(text_substr_2(MakeTextDatum("hello"), Int32GetDatum(2))), "ello");
}

TEST_F(StringFuncsExtTest, Substr2Boundary) {
    // Start = 1 → entire string.
    EXPECT_EQ(TextDatumToString(text_substr_2(MakeTextDatum("hello"), Int32GetDatum(1))), "hello");
    // Start past end → empty.
    EXPECT_EQ(TextDatumToString(text_substr_2(MakeTextDatum("hello"), Int32GetDatum(10))), "");
    // Empty string.
    EXPECT_EQ(TextDatumToString(text_substr_2(MakeTextDatum(""), Int32GetDatum(1))), "");
}

TEST_F(StringFuncsExtTest, Substr2NegativeStart) {
    // Negative start is treated as 1.
    EXPECT_EQ(TextDatumToString(text_substr_2(MakeTextDatum("hello"), Int32GetDatum(-3))), "hello");
}

// ===========================================================================
// text_trim (alias of text_btrim)
// ===========================================================================

TEST_F(StringFuncsExtTest, TrimNormal) {
    EXPECT_EQ(TextDatumToString(text_trim(MakeTextDatum("  hello  "))), "hello");
    EXPECT_EQ(TextDatumToString(text_trim(MakeTextDatum("\t\nhello\r\n"))), "hello");
}

TEST_F(StringFuncsExtTest, TrimBoundary) {
    EXPECT_EQ(TextDatumToString(text_trim(MakeTextDatum(""))), "");
    EXPECT_EQ(TextDatumToString(text_trim(MakeTextDatum("   "))), "");
    EXPECT_EQ(TextDatumToString(text_trim(MakeTextDatum("hello"))), "hello");
}

TEST_F(StringFuncsExtTest, TrimEqualsBtrim) {
    // text_trim is an alias of text_btrim — same input produces same output.
    Datum in = MakeTextDatum("   abc   ");
    EXPECT_EQ(TextDatumToString(text_trim(in)), TextDatumToString(text_btrim(in)));
}

}  // namespace

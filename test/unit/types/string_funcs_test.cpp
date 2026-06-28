#include "pgcpp/types/string_funcs.hpp"

#include <gtest/gtest.h>

#include <string>

#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/alloc_set.hpp"
#include "pgcpp/common/memory/memory_context.hpp"
#include "pgcpp/types/builtins.hpp"
#include "pgcpp/types/datum.hpp"

namespace {

using pgcpp::error::ErrorData;
using pgcpp::error::LogLevel;
using pgcpp::memory::AllocSetContext;
using pgcpp::types::Datum;
using pgcpp::types::DatumGetBool;
using pgcpp::types::DatumGetInt32;
using pgcpp::types::Int32GetDatum;
using pgcpp::types::like;
using pgcpp::types::MakeTextDatum;
using pgcpp::types::not_like;
using pgcpp::types::regexp_replace;
using pgcpp::types::substring;
using pgcpp::types::text_btrim;
using pgcpp::types::text_left;
using pgcpp::types::text_length;
using pgcpp::types::text_lower;
using pgcpp::types::text_ltrim;
using pgcpp::types::text_repeat;
using pgcpp::types::text_reverse;
using pgcpp::types::text_right;
using pgcpp::types::text_rtrim;
using pgcpp::types::text_substr;
using pgcpp::types::text_upper;
using pgcpp::types::textcat;
using pgcpp::types::TextDatumToString;

class StringFuncsTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("string_funcs_test_context");
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
// length
// ===========================================================================

TEST_F(StringFuncsTest, LengthBasic) {
    EXPECT_EQ(DatumGetInt32(text_length(MakeTextDatum("hello"))), 5);
    EXPECT_EQ(DatumGetInt32(text_length(MakeTextDatum(""))), 0);
    EXPECT_EQ(DatumGetInt32(text_length(MakeTextDatum("a"))), 1);
}

TEST_F(StringFuncsTest, LengthUrl) {
    EXPECT_EQ(DatumGetInt32(text_length(MakeTextDatum("http://example.com"))), 18);
}

// ===========================================================================
// LIKE matching
// ===========================================================================

TEST_F(StringFuncsTest, LikeExactMatch) {
    EXPECT_TRUE(DatumGetBool(like(MakeTextDatum("hello"), MakeTextDatum("hello"))));
    EXPECT_FALSE(DatumGetBool(like(MakeTextDatum("hello"), MakeTextDatum("world"))));
}

TEST_F(StringFuncsTest, LikePercentPrefix) {
    // %google% — matches any string containing "google"
    EXPECT_TRUE(
        DatumGetBool(like(MakeTextDatum("http://www.google.com"), MakeTextDatum("%google%"))));
    EXPECT_TRUE(DatumGetBool(like(MakeTextDatum("google.com"), MakeTextDatum("%google%"))));
    EXPECT_TRUE(DatumGetBool(like(MakeTextDatum("www.google"), MakeTextDatum("%google%"))));
    EXPECT_FALSE(DatumGetBool(like(MakeTextDatum("www.yahoo.com"), MakeTextDatum("%google%"))));
}

TEST_F(StringFuncsTest, LikePercentSuffix) {
    EXPECT_TRUE(DatumGetBool(like(MakeTextDatum("hello world"), MakeTextDatum("hello%"))));
    EXPECT_FALSE(DatumGetBool(like(MakeTextDatum("hi world"), MakeTextDatum("hello%"))));
}

TEST_F(StringFuncsTest, LikePercentBoth) {
    EXPECT_TRUE(DatumGetBool(like(MakeTextDatum("hello world"), MakeTextDatum("%world"))));
    EXPECT_TRUE(DatumGetBool(like(MakeTextDatum("world"), MakeTextDatum("%world"))));
    EXPECT_FALSE(DatumGetBool(like(MakeTextDatum("worlds"), MakeTextDatum("%world"))));
}

TEST_F(StringFuncsTest, LikeUnderscore) {
    // _ matches exactly one character
    EXPECT_TRUE(DatumGetBool(like(MakeTextDatum("abc"), MakeTextDatum("a_c"))));
    EXPECT_FALSE(DatumGetBool(like(MakeTextDatum("ac"), MakeTextDatum("a_c"))));
    EXPECT_FALSE(DatumGetBool(like(MakeTextDatum("abbc"), MakeTextDatum("a_c"))));
}

TEST_F(StringFuncsTest, LikeMixedPercentUnderscore) {
    EXPECT_TRUE(DatumGetBool(like(MakeTextDatum("hello"), MakeTextDatum("h_llo"))));
    EXPECT_TRUE(DatumGetBool(like(MakeTextDatum("hello world"), MakeTextDatum("h_llo%"))));
    EXPECT_TRUE(DatumGetBool(like(MakeTextDatum("hxllo world"), MakeTextDatum("h_llo%"))));
    EXPECT_FALSE(DatumGetBool(like(MakeTextDatum("hxxllo"), MakeTextDatum("h_llo%"))));
}

TEST_F(StringFuncsTest, LikeEmptyPattern) {
    EXPECT_TRUE(DatumGetBool(like(MakeTextDatum(""), MakeTextDatum(""))));
    EXPECT_FALSE(DatumGetBool(like(MakeTextDatum("a"), MakeTextDatum(""))));
    EXPECT_TRUE(DatumGetBool(like(MakeTextDatum(""), MakeTextDatum("%"))));
}

TEST_F(StringFuncsTest, LikeOnlyPercent) {
    EXPECT_TRUE(DatumGetBool(like(MakeTextDatum("anything"), MakeTextDatum("%"))));
    EXPECT_TRUE(DatumGetBool(like(MakeTextDatum(""), MakeTextDatum("%"))));
}

TEST_F(StringFuncsTest, LikeMultiplePercent) {
    EXPECT_TRUE(DatumGetBool(like(MakeTextDatum("hello"), MakeTextDatum("%%%"))));
    EXPECT_TRUE(DatumGetBool(like(MakeTextDatum("hello"), MakeTextDatum("h%l%o"))));
    EXPECT_TRUE(DatumGetBool(like(MakeTextDatum("hello"), MakeTextDatum("h%%%o"))));
}

TEST_F(StringFuncsTest, LikeCaseSensitive) {
    // PostgreSQL LIKE is case-sensitive by default
    EXPECT_FALSE(DatumGetBool(like(MakeTextDatum("Hello"), MakeTextDatum("hello"))));
    EXPECT_FALSE(DatumGetBool(like(MakeTextDatum("HELLO"), MakeTextDatum("%hello%"))));
}

TEST_F(StringFuncsTest, LikeEscapeBackslash) {
    // \% matches a literal %
    EXPECT_TRUE(DatumGetBool(like(MakeTextDatum("50%"), MakeTextDatum("50\\%"))));
    EXPECT_FALSE(DatumGetBool(like(MakeTextDatum("50A"), MakeTextDatum("50\\%"))));
}

TEST_F(StringFuncsTest, LikeSpecialCharsInText) {
    // Text containing regex-special characters should match literally in LIKE
    // (LIKE only treats % and _ as wildcards, not . or +)
    EXPECT_TRUE(DatumGetBool(like(MakeTextDatum("a.b+c"), MakeTextDatum("a.b+c"))));
    // %.% matches any text containing a literal '.'
    EXPECT_TRUE(DatumGetBool(like(MakeTextDatum("a.b+c"), MakeTextDatum("%.%"))));
    // %.+% requires literal '.+' in the text; "a.b+c" does not contain it
    EXPECT_FALSE(DatumGetBool(like(MakeTextDatum("a.b+c"), MakeTextDatum("%.+%"))));
    EXPECT_TRUE(DatumGetBool(like(MakeTextDatum("a.+c"), MakeTextDatum("%.+%"))));
}

// ===========================================================================
// NOT LIKE
// ===========================================================================

TEST_F(StringFuncsTest, NotLikeBasic) {
    EXPECT_FALSE(DatumGetBool(not_like(MakeTextDatum("hello"), MakeTextDatum("hello"))));
    EXPECT_TRUE(DatumGetBool(not_like(MakeTextDatum("hello"), MakeTextDatum("world"))));
}

TEST_F(StringFuncsTest, NotLikePercent) {
    // URL NOT LIKE '%.google.%'
    // "http://www.google.com" contains ".google." so LIKE matches, NOT LIKE = false
    EXPECT_FALSE(DatumGetBool(
        not_like(MakeTextDatum("http://www.google.com"), MakeTextDatum("%.google.%"))));
    // "http://www.yahoo.com" does NOT contain ".google." so NOT LIKE = true
    EXPECT_TRUE(
        DatumGetBool(not_like(MakeTextDatum("http://www.yahoo.com"), MakeTextDatum("%.google.%"))));
}

// ===========================================================================
// ClickBench-specific LIKE scenarios
// ===========================================================================

TEST_F(StringFuncsTest, ClickBenchUrlLikeGoogle) {
    // Query 21: URL LIKE '%google%'
    EXPECT_TRUE(DatumGetBool(
        like(MakeTextDatum("http://www.google.com/search"), MakeTextDatum("%google%"))));
    EXPECT_TRUE(DatumGetBool(like(MakeTextDatum("https://google.com"), MakeTextDatum("%google%"))));
    EXPECT_FALSE(
        DatumGetBool(like(MakeTextDatum("https://www.yahoo.com"), MakeTextDatum("%google%"))));
}

TEST_F(StringFuncsTest, ClickBenchTitleLikeGoogle) {
    // Query 23: Title LIKE '%Google%'
    EXPECT_TRUE(DatumGetBool(like(MakeTextDatum("Google Search"), MakeTextDatum("%Google%"))));
    EXPECT_FALSE(DatumGetBool(like(MakeTextDatum("Yahoo Search"), MakeTextDatum("%Google%"))));
}

TEST_F(StringFuncsTest, ClickBenchUrlNotLikeGoogleDomain) {
    // Query 23: URL NOT LIKE '%.google.%'
    EXPECT_TRUE(
        DatumGetBool(not_like(MakeTextDatum("http://www.yahoo.com"), MakeTextDatum("%.google.%"))));
    EXPECT_FALSE(DatumGetBool(
        not_like(MakeTextDatum("http://www.google.com"), MakeTextDatum("%.google.%"))));
}

// ===========================================================================
// regexp_replace
// ===========================================================================

TEST_F(StringFuncsTest, RegexpReplaceBasic) {
    // Replace digits with X
    Datum result =
        regexp_replace(MakeTextDatum("abc123def"), MakeTextDatum("[0-9]+"), MakeTextDatum("X"));
    EXPECT_EQ(TextDatumToString(result), "abcXdef");
}

TEST_F(StringFuncsTest, RegexpReplaceWithCaptureGroup) {
    // ClickBench query 29: extract domain from URL
    // REGEXP_REPLACE(Referer, '^https?://(?:www\.)?([^/]+)/.*$', '\1')
    Datum result =
        regexp_replace(MakeTextDatum("http://www.example.com/page?q=1"),
                       MakeTextDatum("^https?://(?:www\\.)?([^/]+)/.*$"), MakeTextDatum("\\1"));
    EXPECT_EQ(TextDatumToString(result), "example.com");
}

TEST_F(StringFuncsTest, RegexpReplaceHttpsUrl) {
    Datum result =
        regexp_replace(MakeTextDatum("https://www.google.com/search?q=test"),
                       MakeTextDatum("^https?://(?:www\\.)?([^/]+)/.*$"), MakeTextDatum("\\1"));
    EXPECT_EQ(TextDatumToString(result), "google.com");
}

TEST_F(StringFuncsTest, RegexpReplaceNoMatch) {
    // If the pattern doesn't match, the source is returned unchanged.
    Datum result =
        regexp_replace(MakeTextDatum("hello world"), MakeTextDatum("[0-9]+"), MakeTextDatum("X"));
    EXPECT_EQ(TextDatumToString(result), "hello world");
}

TEST_F(StringFuncsTest, RegexpReplaceEmptyString) {
    Datum result = regexp_replace(MakeTextDatum(""), MakeTextDatum("[0-9]+"), MakeTextDatum("X"));
    EXPECT_EQ(TextDatumToString(result), "");
}

TEST_F(StringFuncsTest, RegexpReplaceInvalidPattern) {
    EXPECT_TRUE(RaisesError(
        [] { regexp_replace(MakeTextDatum("test"), MakeTextDatum("["), MakeTextDatum("X")); }));
}

TEST_F(StringFuncsTest, RegexpReplaceSpecialChars) {
    // Replace whitespace
    Datum result =
        regexp_replace(MakeTextDatum("hello world"), MakeTextDatum("\\s+"), MakeTextDatum("_"));
    EXPECT_EQ(TextDatumToString(result), "hello_world");
}

// ===========================================================================
// substring
// ===========================================================================

TEST_F(StringFuncsTest, SubstringBasic) {
    Datum result = substring(MakeTextDatum("hello123world"), MakeTextDatum("[0-9]+"));
    EXPECT_EQ(TextDatumToString(result), "123");
}

TEST_F(StringFuncsTest, SubstringWithCaptureGroup) {
    // Extract the domain from a URL
    Datum result =
        substring(MakeTextDatum("http://example.com/page"), MakeTextDatum("^https?://([^/]+)"));
    EXPECT_EQ(TextDatumToString(result), "example.com");
}

TEST_F(StringFuncsTest, SubstringNoMatch) {
    Datum result = substring(MakeTextDatum("hello world"), MakeTextDatum("[0-9]+"));
    EXPECT_EQ(TextDatumToString(result), "");
}

TEST_F(StringFuncsTest, SubstringInvalidPattern) {
    EXPECT_TRUE(RaisesError([] { substring(MakeTextDatum("test"), MakeTextDatum("(")); }));
}

// ===========================================================================
// regexp_replace — PG semantics (first-match-only by default)
// ===========================================================================

TEST_F(StringFuncsTest, RegexpReplaceFirstOnly) {
    // PG's default (no 'g' flag) replaces ONLY the first match.
    Datum result = regexp_replace(MakeTextDatum("abcabc"), MakeTextDatum("b"), MakeTextDatum("X"));
    EXPECT_EQ(TextDatumToString(result), "aXcabc");
}

TEST_F(StringFuncsTest, RegexpReplaceGlobalFlag) {
    // 4-arg form with 'g' flag replaces ALL matches.
    Datum result = regexp_replace(MakeTextDatum("abcabc"), MakeTextDatum("b"), MakeTextDatum("X"),
                                  MakeTextDatum("g"));
    EXPECT_EQ(TextDatumToString(result), "aXcaXc");
}

TEST_F(StringFuncsTest, RegexpReplaceFourArgNoGFlag) {
    // 4-arg form without 'g' flag also replaces only the first match.
    Datum result = regexp_replace(MakeTextDatum("abcabc"), MakeTextDatum("b"), MakeTextDatum("X"),
                                  MakeTextDatum(""));
    EXPECT_EQ(TextDatumToString(result), "aXcabc");
}

// ===========================================================================
// textcat — alias of text_concat
// ===========================================================================

TEST_F(StringFuncsTest, TextCat) {
    Datum result = textcat(MakeTextDatum("hello "), MakeTextDatum("world"));
    EXPECT_EQ(TextDatumToString(result), "hello world");
}

// ===========================================================================
// text_substr
// ===========================================================================

TEST_F(StringFuncsTest, TextSubstr) {
    // PG's substr(text, from, len) is 1-based.
    Datum result = text_substr(MakeTextDatum("hello world"), Int32GetDatum(1), Int32GetDatum(5));
    EXPECT_EQ(TextDatumToString(result), "hello");
}

TEST_F(StringFuncsTest, TextSubstrFromMiddle) {
    Datum result = text_substr(MakeTextDatum("hello world"), Int32GetDatum(7), Int32GetDatum(5));
    EXPECT_EQ(TextDatumToString(result), "world");
}

TEST_F(StringFuncsTest, TextSubstrBeyondEnd) {
    // If start is past the end, return empty string.
    Datum result = text_substr(MakeTextDatum("abc"), Int32GetDatum(10), Int32GetDatum(3));
    EXPECT_EQ(TextDatumToString(result), "");
}

// ===========================================================================
// text_left / text_right
// ===========================================================================

TEST_F(StringFuncsTest, TextLeft) {
    EXPECT_EQ(TextDatumToString(text_left(MakeTextDatum("hello"), Int32GetDatum(3))), "hel");
    EXPECT_EQ(TextDatumToString(text_left(MakeTextDatum("hello"), Int32GetDatum(10))), "hello");
}

TEST_F(StringFuncsTest, TextRight) {
    EXPECT_EQ(TextDatumToString(text_right(MakeTextDatum("hello"), Int32GetDatum(3))), "llo");
    EXPECT_EQ(TextDatumToString(text_right(MakeTextDatum("hello"), Int32GetDatum(10))), "hello");
}

// ===========================================================================
// text_upper / text_lower
// ===========================================================================

TEST_F(StringFuncsTest, TextUpper) {
    EXPECT_EQ(TextDatumToString(text_upper(MakeTextDatum("hello World"))), "HELLO WORLD");
}

TEST_F(StringFuncsTest, TextLower) {
    EXPECT_EQ(TextDatumToString(text_lower(MakeTextDatum("Hello World"))), "hello world");
}

// ===========================================================================
// text_btrim / text_ltrim / text_rtrim
// ===========================================================================

TEST_F(StringFuncsTest, TextBtrim) {
    EXPECT_EQ(TextDatumToString(text_btrim(MakeTextDatum("  hello  "))), "hello");
    EXPECT_EQ(TextDatumToString(text_btrim(MakeTextDatum("\t\nhello\r\n"))), "hello");
    EXPECT_EQ(TextDatumToString(text_btrim(MakeTextDatum("hello"))), "hello");
}

TEST_F(StringFuncsTest, TextLtrim) {
    EXPECT_EQ(TextDatumToString(text_ltrim(MakeTextDatum("  hello  "))), "hello  ");
    EXPECT_EQ(TextDatumToString(text_ltrim(MakeTextDatum("hello"))), "hello");
}

TEST_F(StringFuncsTest, TextRtrim) {
    EXPECT_EQ(TextDatumToString(text_rtrim(MakeTextDatum("  hello  "))), "  hello");
    EXPECT_EQ(TextDatumToString(text_rtrim(MakeTextDatum("hello"))), "hello");
}

// ===========================================================================
// text_repeat / text_reverse
// ===========================================================================

TEST_F(StringFuncsTest, TextRepeat) {
    EXPECT_EQ(TextDatumToString(text_repeat(MakeTextDatum("ab"), Int32GetDatum(3))), "ababab");
    EXPECT_EQ(TextDatumToString(text_repeat(MakeTextDatum("ab"), Int32GetDatum(0))), "");
    EXPECT_EQ(TextDatumToString(text_repeat(MakeTextDatum("ab"), Int32GetDatum(-1))), "");
}

TEST_F(StringFuncsTest, TextReverse) {
    EXPECT_EQ(TextDatumToString(text_reverse(MakeTextDatum("hello"))), "olleh");
    EXPECT_EQ(TextDatumToString(text_reverse(MakeTextDatum(""))), "");
    EXPECT_EQ(TextDatumToString(text_reverse(MakeTextDatum("a"))), "a");
}

}  // namespace

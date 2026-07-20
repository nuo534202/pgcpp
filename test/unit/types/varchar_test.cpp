// varchar_test.cpp — Unit tests for VARCHAR(n) typmod truncation.
//
// Covers the typmod-aware varchar_in() and varchar_typmod_coerce() functions
// (mirrors PostgreSQL 15's src/backend/utils/adt/varchar.c). Verifies that:
//   - VARCHAR(5) accepts strings up to 5 characters unchanged
//   - VARCHAR(5) truncates trailing spaces when the value is too long
//   - VARCHAR(5) errors when the overflow contains non-space characters
//   - VARCHAR(5) does NOT pad short values (unlike CHAR(5))
//   - VARCHAR without a typmod (-1) accepts any length
//   - The explicit-cast coercion silently truncates 'hello world' to 'hello'
//   - The implicit-cast coercion errors on non-space overflow
//   - Empty strings are handled correctly

#include <gtest/gtest.h>

#include <string>

#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "types/builtins.hpp"
#include "types/datum.hpp"
#include "utils/mb/mbutils.hpp"

namespace {

using pgcpp::error::ErrorData;
using pgcpp::error::LogLevel;
using pgcpp::memory::AllocSetContext;
using pgcpp::types::Datum;
using pgcpp::types::varchar_in;
using pgcpp::types::varchar_out;
using pgcpp::types::varchar_typmod_coerce;

// VARHDRSZ — PostgreSQL's varlena header size. The typmod for VARCHAR(N)
// is encoded as VARHDRSZ + N (so VARCHAR(5) -> typmod 9).
constexpr int32_t kVarHdrSz = 4;

class VarcharTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("varchar_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);
        // varchar_in uses the database encoding for character counting.
        pgcpp::utils::SetDatabaseEncoding(pgcpp::utils::PgEncoding::kUtf8);
    }

    void TearDown() override {
        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    // Helper: build a varchar Datum from a C string with no typmod.
    Datum MakeVarchar(const char* s) { return varchar_in(s); }

    // Helper: extract the C string from a varchar Datum (palloc'd in the
    // current memory context).
    std::string VarcharToString(Datum d) { return varchar_out(d); }

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

    AllocSetContext* context_ = nullptr;
};

// --- varchar_in: no typmod (backwards-compatible default) ---

TEST_F(VarcharTest, NoTypmodAcceptsAnyLength) {
    Datum d = varchar_in("hello world");
    EXPECT_EQ(VarcharToString(d), "hello world");
}

TEST_F(VarcharTest, NoTypmodAcceptsEmptyString) {
    Datum d = varchar_in("");
    EXPECT_EQ(VarcharToString(d), "");
}

TEST_F(VarcharTest, NoTypmodAcceptsNullPtrAsEmpty) {
    Datum d = varchar_in(nullptr);
    EXPECT_EQ(VarcharToString(d), "");
}

// --- varchar_in: VARCHAR(5) boundary cases ---

TEST_F(VarcharTest, Varchar5AcceptsExactLength) {
    // 'hello' is exactly 5 characters — must NOT be truncated.
    Datum d = varchar_in("hello", kVarHdrSz + 5);
    EXPECT_EQ(VarcharToString(d), "hello");
}

TEST_F(VarcharTest, Varchar5AcceptsShortValue) {
    // 'hi' is shorter than 5 — must NOT be padded (varchar is not bpchar).
    Datum d = varchar_in("hi", kVarHdrSz + 5);
    EXPECT_EQ(VarcharToString(d), "hi");
}

TEST_F(VarcharTest, Varchar5AcceptsEmptyString) {
    Datum d = varchar_in("", kVarHdrSz + 5);
    EXPECT_EQ(VarcharToString(d), "");
}

TEST_F(VarcharTest, Varchar5TruncatesTrailingSpaces) {
    // 'hello   ' (5 chars + 3 spaces) — overflow is all spaces, so it is
    // silently truncated to 'hello' (matches PostgreSQL's varcharin).
    Datum d = varchar_in("hello   ", kVarHdrSz + 5);
    EXPECT_EQ(VarcharToString(d), "hello");
}

TEST_F(VarcharTest, Varchar5ErrorsOnNonSpaceOverflow) {
    // 'hello world' — overflow contains non-space characters; must error.
    EXPECT_TRUE(RaisesError([] { varchar_in("hello world", kVarHdrSz + 5); }));
}

TEST_F(VarcharTest, Varchar1ErrorsOnNonSpaceOverflow) {
    EXPECT_TRUE(RaisesError([] { varchar_in("ab", kVarHdrSz + 1); }));
}

TEST_F(VarcharTest, Varchar1AcceptsSingleChar) {
    Datum d = varchar_in("a", kVarHdrSz + 1);
    EXPECT_EQ(VarcharToString(d), "a");
}

// --- varchar_in: VARCHAR(0) and invalid typmod ---

TEST_F(VarcharTest, Varchar0TypmodAcceptsEmptyString) {
    // PostgreSQL requires VARCHAR(N) with N >= 1; the parser rejects 0.
    // If a typmod of VARHDRSZ + 0 = 4 somehow reaches varchar_in, an empty
    // string still fits (0 chars <= 0 max).
    Datum d = varchar_in("", kVarHdrSz + 0);
    EXPECT_EQ(VarcharToString(d), "");
}

TEST_F(VarcharTest, NegativeTypmodSkipsTruncation) {
    // typmod = -1 means "no typmod" — no truncation, like plain text.
    Datum d = varchar_in("hello world", -1);
    EXPECT_EQ(VarcharToString(d), "hello world");
}

// --- varchar_in: multibyte (UTF-8) character handling ---

TEST_F(VarcharTest, Varchar3TruncatesMultibyteToCharBoundary) {
    // "hél   " is 6 characters (h, é, l, space, space, space) but 7 bytes
    // in UTF-8 (é is 2 bytes: 0xC3 0xA9). VARCHAR(3) with all-space overflow
    // must truncate to "hél" (3 chars, 4 bytes) — truncation must respect
    // character boundaries, not byte boundaries.
    std::string input = "h\xc3\xa9l   ";  // hél + 3 spaces
    Datum d = varchar_in(input.c_str(), kVarHdrSz + 3);
    EXPECT_EQ(VarcharToString(d), std::string("h\xc3\xa9l"));  // hél
}

TEST_F(VarcharTest, Varchar3AcceptsExactMultibyteLength) {
    // "hél" is exactly 3 characters (4 bytes) — no truncation.
    std::string input = "h\xc3\xa9l";  // hél
    Datum d = varchar_in(input.c_str(), kVarHdrSz + 3);
    EXPECT_EQ(VarcharToString(d), input);
}

// --- varchar_typmod_coerce: explicit cast (silent truncation) ---

TEST_F(VarcharTest, ExplicitCoerceTruncatesSilently) {
    // Explicit cast 'hello world'::VARCHAR(5) — silently truncates to
    // 'hello'. This is the case from the diff_test_report.md failure.
    Datum source = MakeVarchar("hello world");
    Datum result = varchar_typmod_coerce(source, kVarHdrSz + 5,
                                         /*is_explicit=*/true);
    EXPECT_EQ(VarcharToString(result), "hello");
}

TEST_F(VarcharTest, ExplicitCoerceKeepsShortValue) {
    Datum source = MakeVarchar("hi");
    Datum result = varchar_typmod_coerce(source, kVarHdrSz + 5,
                                         /*is_explicit=*/true);
    EXPECT_EQ(VarcharToString(result), "hi");
}

TEST_F(VarcharTest, ExplicitCoerceKeepsExactLength) {
    Datum source = MakeVarchar("hello");
    Datum result = varchar_typmod_coerce(source, kVarHdrSz + 5,
                                         /*is_explicit=*/true);
    EXPECT_EQ(VarcharToString(result), "hello");
}

TEST_F(VarcharTest, ExplicitCoerceEmptyString) {
    Datum source = MakeVarchar("");
    Datum result = varchar_typmod_coerce(source, kVarHdrSz + 5,
                                         /*is_explicit=*/true);
    EXPECT_EQ(VarcharToString(result), "");
}

TEST_F(VarcharTest, ExplicitCoerceTruncatesTrailingSpaces) {
    Datum source = MakeVarchar("hello   ");
    Datum result = varchar_typmod_coerce(source, kVarHdrSz + 5,
                                         /*is_explicit=*/true);
    EXPECT_EQ(VarcharToString(result), "hello");
}

TEST_F(VarcharTest, ExplicitCoerceInvalidTypmodReturnsSource) {
    Datum source = MakeVarchar("hello world");
    // typmod < VARHDRSZ means "no typmod" — return source unchanged.
    Datum result = varchar_typmod_coerce(source, -1, /*is_explicit=*/true);
    EXPECT_EQ(VarcharToString(result), "hello world");
}

// --- varchar_typmod_coerce: implicit cast (errors on non-space overflow) ---

TEST_F(VarcharTest, ImplicitCoerceErrorsOnNonSpaceOverflow) {
    Datum source = MakeVarchar("hello world");
    EXPECT_TRUE(RaisesError([&] {
        varchar_typmod_coerce(source, kVarHdrSz + 5,
                              /*is_explicit=*/false);
    }));
}

TEST_F(VarcharTest, ImplicitCoerceTruncatesTrailingSpaces) {
    // Implicit cast with all-space overflow is allowed (matches PG).
    Datum source = MakeVarchar("hello   ");
    Datum result = varchar_typmod_coerce(source, kVarHdrSz + 5,
                                         /*is_explicit=*/false);
    EXPECT_EQ(VarcharToString(result), "hello");
}

TEST_F(VarcharTest, ImplicitCoerceKeepsShortValue) {
    Datum source = MakeVarchar("hi");
    Datum result = varchar_typmod_coerce(source, kVarHdrSz + 5,
                                         /*is_explicit=*/false);
    EXPECT_EQ(VarcharToString(result), "hi");
}

// --- varchar_typmod_coerce: multibyte handling ---

TEST_F(VarcharTest, ExplicitCoerceTruncatesMultibyteToCharBoundary) {
    // "héllo" (5 chars, 6 bytes) coerced to VARCHAR(3) -> "hél" (3 chars).
    std::string input = "h\xc3\xa9llo";  // héllo
    Datum source = MakeVarchar(input.c_str());
    Datum result = varchar_typmod_coerce(source, kVarHdrSz + 3,
                                         /*is_explicit=*/true);
    EXPECT_EQ(VarcharToString(result), std::string("h\xc3\xa9l"));  // hél
}

}  // namespace

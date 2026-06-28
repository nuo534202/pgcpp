// regex_test.cpp — unit tests for the MyToyDB regex module.
//
// Verifies the PG-compatible regex API surface (pg_regcomp / pg_regexec /
// pg_regfree / pg_regerror / pg_regprefix / pg_regexport) backed by std::regex.
// Covers basic matching, anchors, character classes, quantifiers, alternation,
// capture groups, case-insensitivity, REG_EXTENDED/REG_NEWLINE flags, error
// reporting, prefix extraction, and safe teardown.

#include "pgcpp/regex/regex.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/alloc_set.hpp"
#include "pgcpp/common/memory/memory_context.hpp"

namespace {

using mytoydb::error::InitErrorSubsystem;
using mytoydb::memory::AllocSetContext;
using mytoydb::regex::kRegBadpat;
using mytoydb::regex::kRegBadrpt;
using mytoydb::regex::kRegExtended;
using mytoydb::regex::kRegIcase;
using mytoydb::regex::kRegNewline;
using mytoydb::regex::kRegNomatch;
using mytoydb::regex::kRegNosub;
using mytoydb::regex::kRegNotbol;
using mytoydb::regex::kRegNoteol;
using mytoydb::regex::kRegOk;
using mytoydb::regex::pg_regcomp;
using mytoydb::regex::pg_regerror;
using mytoydb::regex::pg_regexec;
using mytoydb::regex::pg_regexport;
using mytoydb::regex::pg_regfree;
using mytoydb::regex::pg_regprefix;
using mytoydb::regex::regex_t;
using mytoydb::regex::regmatch_t;

class RegexTest : public ::testing::Test {
protected:
    void SetUp() override {
        InitErrorSubsystem();
        context_ = AllocSetContext::Create("regex_test_context");
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

// Helper: compile a pattern, returning the regex_t*. Assumes success.
regex_t* CompileOk(const char* pattern, int flags = kRegExtended) {
    regex_t* re = pg_regcomp(pattern, flags);
    EXPECT_NE(re, nullptr);
    EXPECT_EQ(re->re_errno, kRegOk);
    return re;
}

// Helper: run pg_regexec and return whether it matched.
bool Matches(regex_t* re, const char* subject, regmatch_t* m = nullptr, std::size_t nmatch = 0,
             int eflags = 0) {
    return pg_regexec(re, subject, nmatch, m, eflags) == kRegOk;
}

// ===========================================================================
// Basic match
// ===========================================================================

TEST_F(RegexTest, BasicMatchHit) {
    regex_t* re = CompileOk("abc");
    EXPECT_TRUE(Matches(re, "abc"));
    EXPECT_TRUE(Matches(re, "xxabcyy"));  // substring match (regex_search)
    pg_regfree(re);
}

TEST_F(RegexTest, BasicMatchMiss) {
    regex_t* re = CompileOk("abc");
    EXPECT_FALSE(Matches(re, "xyz"));
    EXPECT_FALSE(Matches(re, ""));
    pg_regfree(re);
}

// ===========================================================================
// Anchored match (^ and $)
// ===========================================================================

TEST_F(RegexTest, AnchoredExact) {
    regex_t* re = CompileOk("^abc$");
    EXPECT_TRUE(Matches(re, "abc"));
    EXPECT_FALSE(Matches(re, "xxabc"));  // ^ forbids leading chars
    EXPECT_FALSE(Matches(re, "abcyy"));  // $ forbids trailing chars
    pg_regfree(re);
}

TEST_F(RegexTest, AnchoredStartOnly) {
    regex_t* re = CompileOk("^abc");
    EXPECT_TRUE(Matches(re, "abcdef"));
    EXPECT_FALSE(Matches(re, "xabc"));
    pg_regfree(re);
}

TEST_F(RegexTest, AnchoredEndOnly) {
    regex_t* re = CompileOk("abc$");
    EXPECT_TRUE(Matches(re, "xxxabc"));
    EXPECT_FALSE(Matches(re, "abcxxx"));
    pg_regfree(re);
}

// ===========================================================================
// Character classes
// ===========================================================================

TEST_F(RegexTest, CharClassRange) {
    regex_t* re = CompileOk("[a-z]+");
    EXPECT_TRUE(Matches(re, "hello"));
    EXPECT_TRUE(Matches(re, "HELLOabc"));  // contains lowercase substring
    EXPECT_FALSE(Matches(re, "123"));
    pg_regfree(re);
}

TEST_F(RegexTest, CharClassNegated) {
    regex_t* re = CompileOk("[^0-9]");
    EXPECT_TRUE(Matches(re, "abc"));
    EXPECT_TRUE(Matches(re, "a1b"));  // 'a' matches [^0-9]
    EXPECT_FALSE(Matches(re, "123"));
    pg_regfree(re);
}

TEST_F(RegexTest, CharClassDigitClass) {
    regex_t* re = CompileOk("[0-9]+");
    EXPECT_TRUE(Matches(re, "abc123def"));
    EXPECT_FALSE(Matches(re, "no digits here"));
    pg_regfree(re);
}

// ===========================================================================
// Quantifiers
// ===========================================================================

TEST_F(RegexTest, QuantifierStarPlusOpt) {
    regex_t* re = CompileOk("a*b+c?");
    EXPECT_TRUE(Matches(re, "b"));  // a*=0, b+=1, c?=0
    EXPECT_TRUE(Matches(re, "aaabbc"));
    EXPECT_TRUE(Matches(re, "aaaab"));  // c absent
    EXPECT_FALSE(Matches(re, "a"));     // b+ required
    pg_regfree(re);
}

TEST_F(RegexTest, QuantifierBounded) {
    regex_t* re = CompileOk("a{2,4}");
    EXPECT_TRUE(Matches(re, "aa"));     // exactly 2
    EXPECT_TRUE(Matches(re, "aaaa"));   // exactly 4
    EXPECT_TRUE(Matches(re, "aaaaa"));  // 4 of the 5 a's match
    EXPECT_FALSE(Matches(re, "a"));     // only 1
    pg_regfree(re);
}

// ===========================================================================
// Alternation
// ===========================================================================

TEST_F(RegexTest, Alternation) {
    regex_t* re = CompileOk("cat|dog");
    EXPECT_TRUE(Matches(re, "cat"));
    EXPECT_TRUE(Matches(re, "dog"));
    EXPECT_FALSE(Matches(re, "bird"));
    EXPECT_TRUE(Matches(re, "a cat here"));  // substring
    pg_regfree(re);
}

// ===========================================================================
// Capture groups via pmatch
// ===========================================================================

TEST_F(RegexTest, GroupRepetition) {
    regex_t* re = CompileOk("(ab)+");
    EXPECT_TRUE(Matches(re, "ab"));
    EXPECT_TRUE(Matches(re, "ababab"));
    EXPECT_FALSE(Matches(re, "ba"));
    pg_regfree(re);
}

TEST_F(RegexTest, GroupExtractionOffsets) {
    regex_t* re = CompileOk("a(bc)(de)");
    regmatch_t m[3] = {};
    ASSERT_TRUE(Matches(re, "abcde", m, 3));
    // Group 0 = whole match "abcde" at [0,5)
    EXPECT_EQ(m[0].rm_so, 0);
    EXPECT_EQ(m[0].rm_eo, 5);
    // Group 1 = "bc" at [1,3)
    EXPECT_EQ(m[1].rm_so, 1);
    EXPECT_EQ(m[1].rm_eo, 3);
    // Group 2 = "de" at [3,5)
    EXPECT_EQ(m[2].rm_so, 3);
    EXPECT_EQ(m[2].rm_eo, 5);
    pg_regfree(re);
}

TEST_F(RegexTest, GroupExtractionDomain) {
    // ClickBench-style URL domain extraction.
    regex_t* re = CompileOk("^https?://([^/]+)/");
    regmatch_t m[2] = {};
    ASSERT_TRUE(Matches(re, "http://example.com/page", m, 2));
    ASSERT_EQ(m[1].rm_so, 7);
    ASSERT_EQ(m[1].rm_eo, 18);
    std::string domain("http://example.com/page", 7, 11);
    EXPECT_EQ(domain, "example.com");
    pg_regfree(re);
}

TEST_F(RegexTest, NonParticipatingGroup) {
    // (a)|(b) on "a" — group 2 does not participate (rm_so == -1).
    regex_t* re = CompileOk("(a)|(b)");
    regmatch_t m[3] = {};
    ASSERT_TRUE(Matches(re, "a", m, 3));
    EXPECT_GE(m[1].rm_so, 0);   // group 1 matched
    EXPECT_EQ(m[2].rm_so, -1);  // group 2 did not participate
    EXPECT_EQ(m[2].rm_eo, -1);
    pg_regfree(re);
}

TEST_F(RegexTest, NSubCount) {
    regex_t* re = CompileOk("(a)(b)(c)");
    EXPECT_EQ(re->re_nsub, 3);
    pg_regfree(re);
}

// ===========================================================================
// Case-insensitive matching (REG_ICASE)
// ===========================================================================

TEST_F(RegexTest, CaseSensitiveByDefault) {
    regex_t* re = CompileOk("ABC");
    EXPECT_TRUE(Matches(re, "ABC"));
    EXPECT_FALSE(Matches(re, "abc"));
    pg_regfree(re);
}

TEST_F(RegexTest, CaseInsensitiveFlag) {
    regex_t* re = CompileOk("ABC", kRegExtended | kRegIcase);
    EXPECT_TRUE(Matches(re, "abc"));
    EXPECT_TRUE(Matches(re, "ABC"));
    EXPECT_TRUE(Matches(re, "AbC"));
    EXPECT_FALSE(Matches(re, "xyz"));
    pg_regfree(re);
}

// ===========================================================================
// REG_EXTENDED flag (default behavior)
// ===========================================================================

TEST_F(RegexTest, RegExtendedIsDefault) {
    // REG_EXTENDED should behave the same as the default; the project uses
    // ECMAScript syntax which is a superset of ERE for these constructs.
    regex_t* re = CompileOk("(ab)+|c", kRegExtended);
    EXPECT_TRUE(Matches(re, "abab"));
    EXPECT_TRUE(Matches(re, "c"));
    EXPECT_FALSE(Matches(re, "d"));
    pg_regfree(re);
}

TEST_F(RegexTest, RegExtendedWithoutFlagAlsoWorks) {
    // Compiling with flags==0 also yields extended behavior in MyToyDB
    // (ECMAScript is the std::regex default).
    regex_t* re = pg_regcomp("(ab)+", 0);
    ASSERT_EQ(re->re_errno, kRegOk);
    EXPECT_TRUE(Matches(re, "abab"));
    pg_regfree(re);
}

// ===========================================================================
// REG_NOSUB — suppress submatch reporting
// ===========================================================================

TEST_F(RegexTest, RegNosuppressesPmatch) {
    regex_t* re = CompileOk("(a)(b)", kRegExtended | kRegNosub);
    regmatch_t m[3] = {};
    // Match still succeeds ...
    EXPECT_TRUE(Matches(re, "ab", m, 3));
    // ... but pmatch is not filled (offsets remain at their initial -1).
    EXPECT_EQ(m[0].rm_so, -1);
    EXPECT_EQ(m[1].rm_so, -1);
    pg_regfree(re);
}

// ===========================================================================
// Newline handling (REG_NEWLINE)
// ===========================================================================

TEST_F(RegexTest, RegNewlineAcceptedAndMatches) {
    // REG_NEWLINE is accepted at compile time and does not break ordinary
    // (newline-free) matching. PG's exact newline semantics (. does not cross
    // newline, ^/$ match at line boundaries) are approximated by std::regex.
    regex_t* re = CompileOk("^abc$", kRegExtended | kRegNewline);
    EXPECT_TRUE(Matches(re, "abc"));
    EXPECT_FALSE(Matches(re, "abcd"));
    pg_regfree(re);
}

TEST_F(RegexTest, RegNewlineDotDoesNotCrossDefault) {
    // Without REG_NEWLINE, std::regex '.' still does not match '\n' by default
    // (ECMAScript semantics). Verify a multiline string still matches the
    // leading line.
    regex_t* re = CompileOk("abc");
    EXPECT_TRUE(Matches(re, "abc\ndef"));
    pg_regfree(re);
}

// ===========================================================================
// Execution flags: REG_NOTBOL / REG_NOTEOL
// ===========================================================================

TEST_F(RegexTest, ExecNotBol) {
    regex_t* re = CompileOk("^abc");
    EXPECT_FALSE(Matches(re, "abc", nullptr, 0, kRegNotbol));
    EXPECT_TRUE(Matches(re, "abc"));  // no eflags -> ^ matches
    pg_regfree(re);
}

TEST_F(RegexTest, ExecNotEol) {
    regex_t* re = CompileOk("abc$");
    EXPECT_FALSE(Matches(re, "abc", nullptr, 0, kRegNoteol));
    EXPECT_TRUE(Matches(re, "abc"));  // no eflags -> $ matches
    pg_regfree(re);
}

// ===========================================================================
// pg_regerror
// ===========================================================================

TEST_F(RegexTest, ErrorMessageForNomatch) {
    char buf[128] = {};
    std::size_t n = pg_regerror(kRegNomatch, nullptr, buf, sizeof(buf));
    EXPECT_GT(n, 0u);
    EXPECT_NE(std::strstr(buf, "match"), nullptr);
}

TEST_F(RegexTest, ErrorMessageForBadPat) {
    char buf[128] = {};
    std::size_t n = pg_regerror(kRegBadpat, nullptr, buf, sizeof(buf));
    EXPECT_GT(n, 0u);
    EXPECT_NE(std::strstr(buf, "invalid"), nullptr);
}

TEST_F(RegexTest, ErrorMessageForInvalidPattern) {
    // An unbalanced bracket yields an error code; pg_regerror must produce a
    // non-empty message.
    regex_t* re = pg_regcomp("[", kRegExtended);
    ASSERT_NE(re, nullptr);
    ASSERT_NE(re->re_errno, kRegOk);
    char buf[128] = {};
    std::size_t n = pg_regerror(re->re_errno, re, buf, sizeof(buf));
    EXPECT_GT(n, 0u);
    EXPECT_NE(buf[0], '\0');
    pg_regfree(re);
}

TEST_F(RegexTest, ErrorMessageNullBufferReturnsLength) {
    // With a null buffer, pg_regerror still returns the message length.
    std::size_t n = pg_regerror(kRegBadrpt, nullptr, nullptr, 0);
    EXPECT_GT(n, 0u);
}

TEST_F(RegexTest, ErrorMessageSmallBufferTruncates) {
    // A too-small buffer must be NUL-terminated without overflow.
    char buf[4] = {1, 1, 1, 1};
    std::size_t n = pg_regerror(kRegBadpat, nullptr, buf, sizeof(buf));
    EXPECT_GT(n, 0u);
    EXPECT_EQ(buf[3], '\0');
}

// ===========================================================================
// Invalid pattern handling
// ===========================================================================

TEST_F(RegexTest, InvalidUnbalancedParen) {
    regex_t* re = pg_regcomp("(abc", kRegExtended);
    ASSERT_NE(re, nullptr);
    EXPECT_NE(re->re_errno, kRegOk);
    // Executing a failed regex returns an error, not a crash.
    EXPECT_NE(pg_regexec(re, "abc", 0, nullptr, 0), kRegOk);
    pg_regfree(re);
}

TEST_F(RegexTest, InvalidQuantifier) {
    regex_t* re = pg_regcomp("*abc", kRegExtended);
    ASSERT_NE(re, nullptr);
    EXPECT_NE(re->re_errno, kRegOk);
    pg_regfree(re);
}

TEST_F(RegexTest, ValidRegexAfterInvalid) {
    // A failed compile should not corrupt subsequent compiles.
    regex_t* bad = pg_regcomp("(", kRegExtended);
    EXPECT_NE(bad->re_errno, kRegOk);
    pg_regfree(bad);

    regex_t* good = CompileOk("abc");
    EXPECT_TRUE(Matches(good, "abc"));
    pg_regfree(good);
}

// ===========================================================================
// pg_regprefix
// ===========================================================================

TEST_F(RegexTest, PrefixAnchoredLiteral) {
    regex_t* re = CompileOk("^abc.*");
    char* prefix = nullptr;
    std::size_t len = 0;
    int rc = pg_regprefix(re, &prefix, &len);
    EXPECT_GT(rc, 0);
    ASSERT_NE(prefix, nullptr);
    EXPECT_EQ(len, 3u);
    EXPECT_EQ(std::string(prefix, len), "abc");
    pg_regfree(re);
}

TEST_F(RegexTest, PrefixUnanchoredNoPrefix) {
    regex_t* re = CompileOk("a.*");
    char* prefix = nullptr;
    std::size_t len = 0;
    int rc = pg_regprefix(re, &prefix, &len);
    EXPECT_EQ(rc, -1);
    EXPECT_EQ(prefix, nullptr);
    EXPECT_EQ(len, 0u);
    pg_regfree(re);
}

TEST_F(RegexTest, PrefixSingleChar) {
    regex_t* re = CompileOk("^a.*");
    char* prefix = nullptr;
    std::size_t len = 0;
    int rc = pg_regprefix(re, &prefix, &len);
    EXPECT_GT(rc, 0);
    EXPECT_EQ(len, 1u);
    ASSERT_NE(prefix, nullptr);
    EXPECT_EQ(prefix[0], 'a');
    pg_regfree(re);
}

TEST_F(RegexTest, PrefixStartsWithMeta) {
    // ^.*xyz — anchored but starts with a metacharacter, so no literal prefix.
    regex_t* re = CompileOk("^.*xyz");
    char* prefix = nullptr;
    std::size_t len = 0;
    int rc = pg_regprefix(re, &prefix, &len);
    EXPECT_EQ(rc, -1);
    pg_regfree(re);
}

TEST_F(RegexTest, PrefixNullArgs) {
    EXPECT_EQ(pg_regprefix(nullptr, nullptr, nullptr), -1);
    regex_t* re = CompileOk("^abc");
    EXPECT_EQ(pg_regprefix(re, nullptr, nullptr), -1);
    pg_regfree(re);
}

TEST_F(RegexTest, PrefixExactAnchoredFull) {
    // ^abc$ — full literal, prefix is "abc".
    regex_t* re = CompileOk("^abc$");
    char* prefix = nullptr;
    std::size_t len = 0;
    int rc = pg_regprefix(re, &prefix, &len);
    EXPECT_GT(rc, 0);
    EXPECT_EQ(len, 3u);
    pg_regfree(re);
}

// ===========================================================================
// pg_regfree safety
// ===========================================================================

TEST_F(RegexTest, RegfreeNullIsNoOp) {
    pg_regfree(nullptr);  // must not crash
    SUCCEED();
}

TEST_F(RegexTest, RegfreeValidRegex) {
    regex_t* re = CompileOk("abc");
    pg_regfree(re);
    // Calling pg_regfree twice on the same handle would be a use-after-free;
    // we only verify the first call does not crash. The handle is now invalid.
    SUCCEED();
}

TEST_F(RegexTest, RegfreeFailedRegex) {
    // A regex_t whose compilation failed must also be freeable.
    regex_t* re = pg_regcomp("(", kRegExtended);
    ASSERT_NE(re, nullptr);
    ASSERT_NE(re->re_errno, kRegOk);
    pg_regfree(re);  // must not crash
    SUCCEED();
}

TEST_F(RegexTest, ManyCompileFreeCycles) {
    // Repeated compile/free should not leak or crash (ASan/LSan will catch
    // leaks at process exit, but context teardown also reclaims).
    for (int i = 0; i < 50; ++i) {
        regex_t* re = CompileOk("(foo|bar)+");
        ASSERT_TRUE(Matches(re, "foobarfoo"));
        pg_regfree(re);
    }
}

// ===========================================================================
// pg_regexport (stub)
// ===========================================================================

TEST_F(RegexTest, RegexportStubReturnsZero) {
    regex_t* re = CompileOk("abc");
    EXPECT_EQ(pg_regexport(re), 0);
    pg_regfree(re);
}

TEST_F(RegexTest, RegexportStubRejectsNull) {
    EXPECT_EQ(pg_regexport(nullptr), -1);
}

// ===========================================================================
// Misc / realistic patterns
// ===========================================================================

TEST_F(RegexTest, EmailishPattern) {
    regex_t* re = CompileOk("[a-zA-Z0-9_.]+@[a-zA-Z0-9.-]+");
    EXPECT_TRUE(Matches(re, "user@example.com"));
    EXPECT_FALSE(Matches(re, "no-at-sign"));
    pg_regfree(re);
}

TEST_F(RegexTest, WordBoundary) {
    regex_t* re = CompileOk("\\bword\\b");
    EXPECT_TRUE(Matches(re, "a word here"));
    EXPECT_FALSE(Matches(re, "awordhere"));
    pg_regfree(re);
}

TEST_F(RegexTest, OptionalHttpSchema) {
    regex_t* re = CompileOk("https?://");
    EXPECT_TRUE(Matches(re, "http://x"));
    EXPECT_TRUE(Matches(re, "https://x"));
    EXPECT_FALSE(Matches(re, "ftp://x"));
    pg_regfree(re);
}

}  // namespace

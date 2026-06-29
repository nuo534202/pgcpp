// tsearch_test.cpp — unit tests for the M15.20.6 full-text search module.
//
// Covers tsvector/tsquery parsing, tokenizer, dictionaries, thesaurus,
// ispell, the to_tsvector/to_tsquery pipelines, ranking, headline, rewrite,
// and ts_typanalyze. Uses the AllocSetContext fixture pattern so that any
// palloc'd C++ objects are cleaned up via the registered-destructor hook.

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "tsearch/dict.hpp"
#include "tsearch/ispell.hpp"
#include "tsearch/thesaurus.hpp"
#include "tsearch/to_tsquery.hpp"
#include "tsearch/to_tsvector.hpp"
#include "tsearch/ts_typanalyze.hpp"
#include "tsearch/ts_utils.hpp"
#include "tsearch/tsquery_parser.hpp"
#include "tsearch/tsvector_parser.hpp"
#include "tsearch/wparser.hpp"
#include "types/ts_types.hpp"

namespace {

using pgcpp::error::ErrorData;
using pgcpp::error::LogLevel;
using pgcpp::memory::AllocSetContext;
using pgcpp::tsearch::IDictionary;
using pgcpp::tsearch::IspellDict;
using pgcpp::tsearch::Lexeme;
using pgcpp::tsearch::RewriteRule;
using pgcpp::tsearch::SimpleDict;
using pgcpp::tsearch::StopWordsDict;
using pgcpp::tsearch::Thesaurus;
using pgcpp::tsearch::Token;
using pgcpp::tsearch::TokenizeText;
using pgcpp::tsearch::ToTsQuery;
using pgcpp::tsearch::ToTsVector;
using pgcpp::tsearch::TsQueryParse;
using pgcpp::tsearch::TsVectorAnalyze;
using pgcpp::tsearch::TsVectorParse;
using pgcpp::tsearch::TsVectorStats;
using pgcpp::tsearch::WordEntry;
using pgcpp::types::TsQueryData;
using pgcpp::types::TsQueryNode;
using pgcpp::types::TsQueryNodeType;
using pgcpp::types::TsVectorData;

class TsearchTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("tsearch_test_context");
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
// TsVectorParser
// ===========================================================================

TEST_F(TsearchTest, TsVectorParsePositionsAndWeights) {
    auto entries = TsVectorParse("word:1A,2 word2:3");
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].lexeme, "word");
    ASSERT_EQ(entries[0].positions.size(), 2u);
    EXPECT_EQ(entries[0].positions[0], 1);
    EXPECT_EQ(entries[0].positions[1], 2);
    ASSERT_EQ(entries[0].weights.size(), 2u);
    EXPECT_EQ(entries[0].weights[0], 'A');
    EXPECT_EQ(entries[0].weights[1], 'D');
    EXPECT_EQ(entries[1].lexeme, "word2");
    ASSERT_EQ(entries[1].positions.size(), 1u);
    EXPECT_EQ(entries[1].positions[0], 3);
    EXPECT_EQ(entries[1].weights[0], 'D');
}

TEST_F(TsearchTest, TsVectorParseBareLexeme) {
    auto entries = TsVectorParse("hello world");
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].lexeme, "hello");
    EXPECT_TRUE(entries[0].positions.empty());
    EXPECT_EQ(entries[1].lexeme, "world");
}

TEST_F(TsearchTest, TsVectorParseLowercases) {
    auto entries = TsVectorParse("Hello WORLD");
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].lexeme, "hello");
    EXPECT_EQ(entries[1].lexeme, "world");
}

TEST_F(TsearchTest, TsVectorParseInvalidReporits) {
    EXPECT_TRUE(RaisesError([] { TsVectorParse("hello:!bad"); }));
}

// ===========================================================================
// TsQueryParser
// ===========================================================================

TEST_F(TsearchTest, TsQueryParseAnd) {
    auto root = TsQueryParse("a & b");
    EXPECT_EQ(root.type, TsQueryNodeType::kAnd);
    ASSERT_EQ(root.children.size(), 2u);
    EXPECT_EQ(root.children[0].type, TsQueryNodeType::kTerm);
    EXPECT_EQ(root.children[0].lexeme, "a");
    EXPECT_EQ(root.children[1].type, TsQueryNodeType::kTerm);
    EXPECT_EQ(root.children[1].lexeme, "b");
}

TEST_F(TsearchTest, TsQueryParseOr) {
    auto root = TsQueryParse("a | b");
    EXPECT_EQ(root.type, TsQueryNodeType::kOr);
    ASSERT_EQ(root.children.size(), 2u);
    EXPECT_EQ(root.children[0].lexeme, "a");
    EXPECT_EQ(root.children[1].lexeme, "b");
}

TEST_F(TsearchTest, TsQueryParseNot) {
    auto root = TsQueryParse("!a");
    EXPECT_EQ(root.type, TsQueryNodeType::kNot);
    ASSERT_EQ(root.children.size(), 1u);
    EXPECT_EQ(root.children[0].type, TsQueryNodeType::kTerm);
    EXPECT_EQ(root.children[0].lexeme, "a");
}

TEST_F(TsearchTest, TsQueryParseNestedParens) {
    auto root = TsQueryParse("a & (b | c)");
    EXPECT_EQ(root.type, TsQueryNodeType::kAnd);
    ASSERT_EQ(root.children.size(), 2u);
    EXPECT_EQ(root.children[0].lexeme, "a");
    EXPECT_EQ(root.children[1].type, TsQueryNodeType::kOr);
    ASSERT_EQ(root.children[1].children.size(), 2u);
    EXPECT_EQ(root.children[1].children[0].lexeme, "b");
    EXPECT_EQ(root.children[1].children[1].lexeme, "c");
}

TEST_F(TsearchTest, TsQueryParsePrecedence) {
    // a & b | c == (a & b) | c
    auto root = TsQueryParse("a & b | c");
    EXPECT_EQ(root.type, TsQueryNodeType::kOr);
    ASSERT_EQ(root.children.size(), 2u);
    EXPECT_EQ(root.children[0].type, TsQueryNodeType::kAnd);
    EXPECT_EQ(root.children[1].lexeme, "c");
}

TEST_F(TsearchTest, TsQueryParseChainedAndFolds) {
    auto root = TsQueryParse("a & b & c");
    EXPECT_EQ(root.type, TsQueryNodeType::kAnd);
    ASSERT_EQ(root.children.size(), 3u);
}

TEST_F(TsearchTest, TsQueryParseNotBindsTighterThanAnd) {
    // !a & b == (!a) & b
    auto root = TsQueryParse("!a & b");
    EXPECT_EQ(root.type, TsQueryNodeType::kAnd);
    ASSERT_EQ(root.children.size(), 2u);
    EXPECT_EQ(root.children[0].type, TsQueryNodeType::kNot);
    EXPECT_EQ(root.children[1].lexeme, "b");
}

TEST_F(TsearchTest, TsQueryParseInvalidTrailingReporits) {
    EXPECT_TRUE(RaisesError([] { TsQueryParse("a &"); }));
}

// ===========================================================================
// TokenizeText
// ===========================================================================

TEST_F(TsearchTest, TokenizeSplitsPunctuation) {
    auto tokens = TokenizeText("Hello, world!");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].text, "hello");
    EXPECT_EQ(tokens[0].position, 1);
    EXPECT_EQ(tokens[1].text, "world");
    EXPECT_EQ(tokens[1].position, 2);
}

TEST_F(TsearchTest, TokenizeEmpty) {
    auto tokens = TokenizeText("");
    EXPECT_TRUE(tokens.empty());
}

TEST_F(TsearchTest, TokenizeSkipsLeadingWhitespace) {
    auto tokens = TokenizeText("   hi  there");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].position, 1);
    EXPECT_EQ(tokens[1].position, 2);
}

TEST_F(TsearchTest, TokenizeDefaultWeight) {
    auto tokens = TokenizeText("alpha beta");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].weight, 'D');
}

// ===========================================================================
// SimpleDict
// ===========================================================================

TEST_F(TsearchTest, SimpleDictLowercases) {
    SimpleDict dict;
    Lexeme lex = dict.Lexicalize("HELLO");
    EXPECT_EQ(lex.text, "hello");
    EXPECT_FALSE(lex.is_stop);
}

TEST_F(TsearchTest, SimpleDictStripsIng) {
    SimpleDict dict;
    EXPECT_EQ(dict.Lexicalize("running").text, "runn");
}

TEST_F(TsearchTest, SimpleDictStripsIesToY) {
    SimpleDict dict;
    // "stories" -> "stor" + "y" = "story"
    EXPECT_EQ(dict.Lexicalize("stories").text, "story");
}

TEST_F(TsearchTest, SimpleDictStripsEd) {
    SimpleDict dict;
    EXPECT_EQ(dict.Lexicalize("jumped").text, "jump");
}

TEST_F(TsearchTest, SimpleDictStripsS) {
    SimpleDict dict;
    // "cats" -> "cat"
    EXPECT_EQ(dict.Lexicalize("cats").text, "cat");
}

// ===========================================================================
// StopWordsDict
// ===========================================================================

TEST_F(TsearchTest, StopWordsFiltersCommonWords) {
    StopWordsDict dict;
    EXPECT_TRUE(dict.Lexicalize("the").is_stop);
    EXPECT_TRUE(dict.Lexicalize("THE").is_stop);  // case-insensitive
    EXPECT_TRUE(dict.Lexicalize("a").is_stop);
    EXPECT_TRUE(dict.Lexicalize("is").is_stop);
    EXPECT_FALSE(dict.Lexicalize("cat").is_stop);
}

TEST_F(TsearchTest, StopWordsPreservesText) {
    StopWordsDict dict;
    Lexeme lex = dict.Lexicalize("Cat");
    EXPECT_EQ(lex.text, "cat");
    EXPECT_FALSE(lex.is_stop);
}

// ===========================================================================
// Thesaurus
// ===========================================================================

TEST_F(TsearchTest, ThesaurusLookupReturnsSynonyms) {
    Thesaurus th;
    th.AddSynonyms({"fast", "quick", "rapid"});
    auto syns = th.Lookup("quick");
    ASSERT_EQ(syns.size(), 3u);
    // The group should contain all three synonyms.
    EXPECT_NE(std::find(syns.begin(), syns.end(), "fast"), syns.end());
    EXPECT_NE(std::find(syns.begin(), syns.end(), "quick"), syns.end());
    EXPECT_NE(std::find(syns.begin(), syns.end(), "rapid"), syns.end());
}

TEST_F(TsearchTest, ThesaurusLookupCaseInsensitive) {
    Thesaurus th;
    th.AddSynonyms({"big", "large"});
    auto syns = th.Lookup("BIG");
    ASSERT_EQ(syns.size(), 2u);
    EXPECT_NE(std::find(syns.begin(), syns.end(), "big"), syns.end());
}

TEST_F(TsearchTest, ThesaurusLookupUnknownReturnsInput) {
    Thesaurus th;
    auto syns = th.Lookup("unknown");
    ASSERT_EQ(syns.size(), 1u);
    EXPECT_EQ(syns[0], "unknown");
}

TEST_F(TsearchTest, ThesaurusSize) {
    Thesaurus th;
    EXPECT_EQ(th.Size(), 0u);
    th.AddSynonyms({"a", "b"});
    EXPECT_EQ(th.Size(), 1u);
}

// ===========================================================================
// IspellDict
// ===========================================================================

TEST_F(TsearchTest, IspellFindsStem) {
    IspellDict dict;
    dict.AddWord("play");
    dict.AddAffix("ed", false);
    // "played" -> strip "ed" -> "play" matches stored word.
    EXPECT_EQ(dict.Lexicalize("played").text, "play");
}

TEST_F(TsearchTest, IspellPrefixStripping) {
    IspellDict dict;
    dict.AddWord("happy");
    dict.AddAffix("un", true);
    EXPECT_EQ(dict.Lexicalize("unhappy").text, "happy");
}

TEST_F(TsearchTest, IspellFallsBackToLowercased) {
    IspellDict dict;
    dict.AddWord("run");
    dict.AddAffix("ing", false);
    // "Walking" — no matching stem/affix; returns lowercased word.
    EXPECT_EQ(dict.Lexicalize("Walking").text, "walking");
}

TEST_F(TsearchTest, IspellCounts) {
    IspellDict dict;
    dict.AddWord("run");
    dict.AddWord("walk");
    dict.AddAffix("ing", false);
    EXPECT_EQ(dict.WordCount(), 2u);
    EXPECT_EQ(dict.AffixCount(), 1u);
}

// ===========================================================================
// ToTsVector
// ===========================================================================

TEST_F(TsearchTest, ToTsVectorRemovesStopWords) {
    auto vec = ToTsVector("The quick brown fox", "english");
    std::vector<std::string> lexemes;
    for (const auto& e : vec.entries) {
        lexemes.push_back(e.lexeme);
    }
    // "the" should be filtered.
    EXPECT_EQ(std::find(lexemes.begin(), lexemes.end(), "the"), lexemes.end());
    // The other three should survive (after stemming).
    EXPECT_GE(lexemes.size(), 3u);
    EXPECT_NE(std::find(lexemes.begin(), lexemes.end(), "quick"), lexemes.end());
    EXPECT_NE(std::find(lexemes.begin(), lexemes.end(), "brown"), lexemes.end());
    EXPECT_NE(std::find(lexemes.begin(), lexemes.end(), "fox"), lexemes.end());
}

TEST_F(TsearchTest, ToTsVectorSimpleConfigKeepsStopWords) {
    auto vec = ToTsVector("the cat", "simple");
    std::vector<std::string> lexemes;
    for (const auto& e : vec.entries) {
        lexemes.push_back(e.lexeme);
    }
    // "simple" config: stop words are kept.
    EXPECT_NE(std::find(lexemes.begin(), lexemes.end(), "the"), lexemes.end());
    EXPECT_NE(std::find(lexemes.begin(), lexemes.end(), "cat"), lexemes.end());
}

TEST_F(TsearchTest, ToTsVectorSortsLexemes) {
    auto vec = ToTsVector("zebra apple mango", "simple");
    ASSERT_GE(vec.entries.size(), 3u);
    EXPECT_EQ(vec.entries[0].lexeme, "apple");
    EXPECT_EQ(vec.entries[1].lexeme, "mango");
    EXPECT_EQ(vec.entries[2].lexeme, "zebra");
}

TEST_F(TsearchTest, ToTsVectorDeduplicatesLexemes) {
    auto vec = ToTsVector("cat cat cat", "simple");
    ASSERT_EQ(vec.entries.size(), 1u);
    EXPECT_EQ(vec.entries[0].lexeme, "cat");
    ASSERT_EQ(vec.entries[0].positions.size(), 3u);
}

TEST_F(TsearchTest, ToTsVectorStems) {
    auto vec = ToTsVector("running cats", "english");
    std::vector<std::string> lexemes;
    for (const auto& e : vec.entries) {
        lexemes.push_back(e.lexeme);
    }
    // "running" -> SimpleDict strips "ing" -> "runn"
    EXPECT_NE(std::find(lexemes.begin(), lexemes.end(), "runn"), lexemes.end());
    // "cats" -> "cat"
    EXPECT_NE(std::find(lexemes.begin(), lexemes.end(), "cat"), lexemes.end());
}

// ===========================================================================
// ToTsQuery
// ===========================================================================

TEST_F(TsearchTest, ToTsQueryBuildsAndChain) {
    auto q = ToTsQuery("quick fox", "simple");
    EXPECT_EQ(q.root.type, TsQueryNodeType::kAnd);
    ASSERT_EQ(q.root.children.size(), 2u);
}

TEST_F(TsearchTest, ToTsQuerySingleTermNoAnd) {
    auto q = ToTsQuery("solo", "simple");
    EXPECT_EQ(q.root.type, TsQueryNodeType::kTerm);
    EXPECT_EQ(q.root.lexeme, "solo");
}

TEST_F(TsearchTest, ToTsQueryRemovesStopWords) {
    auto q = ToTsQuery("the quick", "english");
    // "the" is filtered, so only "quick" survives.
    EXPECT_EQ(q.root.type, TsQueryNodeType::kTerm);
}

TEST_F(TsearchTest, ToTsQueryEmptyText) {
    auto q = ToTsQuery("", "english");
    EXPECT_EQ(q.root.type, TsQueryNodeType::kTerm);
    EXPECT_TRUE(q.root.lexeme.empty());
}

// ===========================================================================
// ts_rank / ts_rank_cd
// ===========================================================================

TEST_F(TsearchTest, TsRankMatchingBeatsNonMatching) {
    auto vec_match = ToTsVector("hello world", "simple");
    auto vec_miss = ToTsVector("alpha beta", "simple");
    auto q = ToTsQuery("hello", "simple");
    float rank_match = pgcpp::tsearch::ts_rank(vec_match, q);
    float rank_miss = pgcpp::tsearch::ts_rank(vec_miss, q);
    EXPECT_GT(rank_match, 0.0f);
    EXPECT_EQ(rank_miss, 0.0f);
    EXPECT_GT(rank_match, rank_miss);
}

TEST_F(TsearchTest, TsRankCDDenserBeatsSparser) {
    // Both vectors match the same query, but the first has positions closer.
    TsVectorData dense;
    dense.entries.push_back({"alpha", {1}});
    dense.entries.push_back({"beta", {2}});
    TsVectorData sparse;
    sparse.entries.push_back({"alpha", {1}});
    sparse.entries.push_back({"beta", {100}});
    TsQueryData q;
    q.root.type = TsQueryNodeType::kAnd;
    TsQueryNode a, b;
    a.type = TsQueryNodeType::kTerm;
    a.lexeme = "alpha";
    b.type = TsQueryNodeType::kTerm;
    b.lexeme = "beta";
    q.root.children.push_back(a);
    q.root.children.push_back(b);
    float dense_score = pgcpp::tsearch::ts_rank_cd(dense, q);
    float sparse_score = pgcpp::tsearch::ts_rank_cd(sparse, q);
    EXPECT_GT(dense_score, sparse_score);
}

TEST_F(TsearchTest, TsRankEmptyQueryIsZero) {
    auto vec = ToTsVector("hello world", "simple");
    TsQueryData q;
    q.root.type = TsQueryNodeType::kTerm;
    q.root.lexeme = "nomatch";
    EXPECT_EQ(pgcpp::tsearch::ts_rank(vec, q), 0.0f);
}

// ===========================================================================
// ts_headline
// ===========================================================================

TEST_F(TsearchTest, TsHeadlineWrapsMatch) {
    std::string text = "the quick brown fox jumps over the lazy dog";
    TsQueryData q;
    q.root.type = TsQueryNodeType::kTerm;
    q.root.lexeme = "fox";
    std::string hl = pgcpp::tsearch::ts_headline(text, q, 5, "<b>", "</b>");
    EXPECT_NE(hl.find("<b>fox</b>"), std::string::npos);
}

TEST_F(TsearchTest, TsHeadlineNoMatchReturnsPrefix) {
    std::string text = "alpha beta gamma";
    TsQueryData q;
    q.root.type = TsQueryNodeType::kTerm;
    q.root.lexeme = "zzz";
    std::string hl = pgcpp::tsearch::ts_headline(text, q, 10, "<b>", "</b>");
    // No match: should not contain highlight markers.
    EXPECT_EQ(hl.find("<b>"), std::string::npos);
}

TEST_F(TsearchTest, TsHeadlineCaseInsensitiveMatch) {
    std::string text = "The QUICK Fox";
    TsQueryData q;
    q.root.type = TsQueryNodeType::kTerm;
    q.root.lexeme = "fox";
    std::string hl = pgcpp::tsearch::ts_headline(text, q, 5, "<b>", "</b>");
    EXPECT_NE(hl.find("<b>Fox</b>"), std::string::npos);
}

// ===========================================================================
// ts_rewrite
// ===========================================================================

TEST_F(TsearchTest, TsRewriteSubstitutesLeaf) {
    auto root = TsQueryParse("a & b");
    RewriteRule rule;
    rule.target_lexeme = "a";
    rule.replacement.type = TsQueryNodeType::kTerm;
    rule.replacement.lexeme = "alpha";
    auto rewritten = pgcpp::tsearch::ts_rewrite(root, {rule});
    EXPECT_EQ(rewritten.type, TsQueryNodeType::kAnd);
    ASSERT_EQ(rewritten.children.size(), 2u);
    EXPECT_EQ(rewritten.children[0].lexeme, "alpha");
    EXPECT_EQ(rewritten.children[1].lexeme, "b");
}

TEST_F(TsearchTest, TsRewritePreservesNonMatching) {
    auto root = TsQueryParse("a & b");
    RewriteRule rule;
    rule.target_lexeme = "z";  // not present
    rule.replacement.type = TsQueryNodeType::kTerm;
    rule.replacement.lexeme = "zeta";
    auto rewritten = pgcpp::tsearch::ts_rewrite(root, {rule});
    EXPECT_EQ(rewritten.children[0].lexeme, "a");
    EXPECT_EQ(rewritten.children[1].lexeme, "b");
}

TEST_F(TsearchTest, TsRewriteReplacesWithSubtree) {
    auto root = TsQueryParse("a");
    RewriteRule rule;
    rule.target_lexeme = "a";
    rule.replacement.type = TsQueryNodeType::kOr;
    TsQueryNode l, r;
    l.type = TsQueryNodeType::kTerm;
    l.lexeme = "alpha";
    r.type = TsQueryNodeType::kTerm;
    r.lexeme = "aleph";
    rule.replacement.children.push_back(l);
    rule.replacement.children.push_back(r);
    auto rewritten = pgcpp::tsearch::ts_rewrite(root, {rule});
    EXPECT_EQ(rewritten.type, TsQueryNodeType::kOr);
    ASSERT_EQ(rewritten.children.size(), 2u);
}

// ===========================================================================
// ts_typanalyze
// ===========================================================================

TEST_F(TsearchTest, TsVectorAnalyzeMostCommon) {
    TsVectorData v1 = ToTsVector("cat dog cat", "simple");
    TsVectorData v2 = ToTsVector("cat bird", "simple");
    std::vector<const TsVectorData*> samples = {&v1, &v2};
    auto stats = TsVectorAnalyze(samples);
    EXPECT_EQ(stats.sample_count, 2u);
    ASSERT_FALSE(stats.most_common_lexemes.empty());
    // "cat" appears in both vectors (3 total occurrences), so it should be #1.
    EXPECT_EQ(stats.most_common_lexemes[0].lexeme, "cat");
    EXPECT_EQ(stats.most_common_lexemes[0].count, 2u);  // 2 vectors contain it
}

TEST_F(TsearchTest, TsVectorAnalyzeAverageLength) {
    TsVectorData v1 = ToTsVector("a b c", "simple");  // 3 entries
    TsVectorData v2 = ToTsVector("d e", "simple");    // 2 entries
    std::vector<const TsVectorData*> samples = {&v1, &v2};
    auto stats = TsVectorAnalyze(samples);
    EXPECT_NEAR(stats.average_length, 2.5, 0.001);
}

TEST_F(TsearchTest, TsVectorAnalyzeEmptySamples) {
    std::vector<const TsVectorData*> samples;
    auto stats = TsVectorAnalyze(samples);
    EXPECT_EQ(stats.sample_count, 0u);
    EXPECT_EQ(stats.average_length, 0.0);
    EXPECT_TRUE(stats.most_common_lexemes.empty());
}

}  // namespace

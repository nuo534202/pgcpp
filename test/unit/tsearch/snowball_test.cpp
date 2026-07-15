// snowball_test.cpp — unit tests for the P3-8 Snowball stemmer, SnowballDict,
// the TsearchConfigRegistry, and the catalog row types.
//
// Covers:
//   - Porter/Snowball stemmer step-by-step (1a, 1b, 1c, 2, 3, 4, 5a, 5b)
//   - Known word pairs from the Porter (1980) paper vocabulary
//   - SnowballDict with and without stop-word filtering
//   - TsearchConfigRegistry registration, lookup, chain building
//   - ApplyDictionaryChain pipeline semantics
//   - ToTsVector / ToTsQuery integration with the snowball-backed "english"
//     configuration
//   - Catalog row type (pg_ts_dict / pg_ts_cfg / pg_ts_template / pg_ts_parser)
//     default initialization

#include "tsearch/snowball.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "catalog/pg_ts_cfg.hpp"
#include "catalog/pg_ts_dict.hpp"
#include "catalog/pg_ts_parser.hpp"
#include "catalog/pg_ts_template.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "tsearch/dict.hpp"
#include "tsearch/to_tsquery.hpp"
#include "tsearch/to_tsvector.hpp"
#include "tsearch/ts_config.hpp"
#include "tsearch/wparser.hpp"
#include "types/ts_types.hpp"

namespace {

using pgcpp::catalog::FormData_pg_ts_cfg;
using pgcpp::catalog::FormData_pg_ts_dict;
using pgcpp::catalog::FormData_pg_ts_parser;
using pgcpp::catalog::FormData_pg_ts_template;
using pgcpp::catalog::kInvalidOid;
using pgcpp::error::InitErrorSubsystem;
using pgcpp::memory::AllocSetContext;
using pgcpp::tsearch::ApplyDictionaryChain;
using pgcpp::tsearch::GetTsearchConfigRegistry;
using pgcpp::tsearch::Lexeme;
using pgcpp::tsearch::RegisterBuiltinTsearchConfigs;
using pgcpp::tsearch::SnowballDict;
using pgcpp::tsearch::SnowballStemmer;
using pgcpp::tsearch::ToTsQuery;
using pgcpp::tsearch::ToTsVector;
using pgcpp::tsearch::TsearchConfigRegistry;
using pgcpp::types::TsVectorData;

class SnowballTest : public ::testing::Test {
protected:
    void SetUp() override {
        InitErrorSubsystem();
        context_ = AllocSetContext::Create("snowball_test_context");
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
// SnowballStemmer — Step 1a (plurals)
// ===========================================================================

TEST_F(SnowballTest, Step1aSsesToSs) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("caresses"), "caress");
}

TEST_F(SnowballTest, Step1aIesToI) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("ponies"), "poni");
    EXPECT_EQ(s.Stem("ties"), "ti");
}

TEST_F(SnowballTest, Step1aSsUnchanged) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("caress"), "caress");
    EXPECT_EQ(s.Stem("class"), "class");
}

TEST_F(SnowballTest, Step1aStripsTrailingS) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("cats"), "cat");
}

// ===========================================================================
// SnowballStemmer — Step 1b (past tense / gerund)
// ===========================================================================

TEST_F(SnowballTest, Step1bEedToEeIfMgt0) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("agreed"), "agre");  // m("agr")=1 > 0 -> "agree" -> 5a -> "agre"
    EXPECT_EQ(s.Stem("feed"), "feed");    // m("f")=0 -> unchanged
}

TEST_F(SnowballTest, Step1bEdRemovedIfVowel) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("plastered"), "plaster");
}

TEST_F(SnowballTest, Step1bIngRemovedIfVowel) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("motoring"), "motor");
}

TEST_F(SnowballTest, Step1bEdKeptIfNoVowel) {
    SnowballStemmer s;
    // "sing" ends in "ing" but stem "s" has no vowel -> unchanged.
    EXPECT_EQ(s.Stem("sing"), "sing");
}

TEST_F(SnowballTest, Step1bPostCleanupAtToAte) {
    SnowballStemmer s;
    // "conflated" -> remove "ed" -> "conflat" -> AT->ATE -> "conflate"
    // -> step5a removes 'e' (m>1) -> "conflat"
    EXPECT_EQ(s.Stem("conflated"), "conflat");
}

TEST_F(SnowballTest, Step1bPostCleanupBlToBle) {
    SnowballStemmer s;
    // "troubled" -> remove "ed" -> "troubl" -> BL->BLE -> "trouble"
    // -> step5a removes 'e' (m=1, not *o) -> "troubl"
    EXPECT_EQ(s.Stem("troubled"), "troubl");
}

TEST_F(SnowballTest, Step1bPostCleanupIzToIze) {
    SnowballStemmer s;
    // "sized" -> remove "ed" -> "siz" -> IZ->IZE -> "size"
    // -> step5a: m=1, *o (cvc, z not w/x/y) -> keep 'e' -> "size"
    EXPECT_EQ(s.Stem("sized"), "size");
}

TEST_F(SnowballTest, Step1bPostCleanupDoubleConsonant) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("hopping"), "hop");   // pp -> p
    EXPECT_EQ(s.Stem("tanned"), "tan");    // nn -> n
    EXPECT_EQ(s.Stem("falling"), "fall");  // ll -> kept (l excluded)
    EXPECT_EQ(s.Stem("hissing"), "hiss");  // ss -> kept (s excluded)
    EXPECT_EQ(s.Stem("fizzed"), "fizz");   // zz -> kept (z excluded)
}

TEST_F(SnowballTest, Step1bPostCleanupCvcAddsE) {
    SnowballStemmer s;
    // "filing" -> remove "ing" -> "fil" -> m=1 *o (cvc) -> add 'e' -> "file"
    EXPECT_EQ(s.Stem("filing"), "file");
    // "failing" -> remove "ing" -> "fail" -> m=1, not *o (l is consonant but
    // "fail" ends in cvc? f(c)a(v)i(v)l(c)... wait. "fail" = f,a,i,l.
    // EndsCvc("fail"): b[3]='l' consonant, b[2]='i' vowel, b[1]='a' vowel ->
    // not cvc (need CVC). So *o=false, no 'e' added -> "fail"
    EXPECT_EQ(s.Stem("failing"), "fail");
}

// ===========================================================================
// SnowballStemmer — Step 1c (y -> i)
// ===========================================================================

TEST_F(SnowballTest, Step1cYToI) {
    SnowballStemmer s;
    // "happy" -> y preceded by vowel (a) -> y becomes i -> "happi"
    EXPECT_EQ(s.Stem("happy"), "happi");
    // "fly" -> y preceded by consonant (l) -> y stays... but "fly" is 3 chars.
    // Actually Porter 1c: (*v*) y -> i. "fly": stem before 'y' is "fl", which
    // has no vowel -> y stays. But "fly" is length 3 > 2, so processed.
    // HasVowel("fl")? f,l are consonants -> false. So y stays -> "fly"
    EXPECT_EQ(s.Stem("fly"), "fly");
}

// ===========================================================================
// SnowballStemmer — Step 2 (m>0 suffix transforms)
// ===========================================================================

TEST_F(SnowballTest, Step2AtionalToAte) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("relational"), "relat");
}

TEST_F(SnowballTest, Step2TionalToTion) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("conditional"), "condit");
    EXPECT_EQ(s.Stem("rational"), "ration");
}

TEST_F(SnowballTest, Step2EnciToEnce) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("valenci"), "valenc");
}

TEST_F(SnowballTest, Step2AnchiToAnce) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("hesitanci"), "hesit");
}

TEST_F(SnowballTest, Step2IzerToIze) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("digitizer"), "digit");
}

TEST_F(SnowballTest, Step2AbliToAble) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("conformabli"), "conform");
}

TEST_F(SnowballTest, Step2AlliToAl) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("radicalli"), "radic");
}

TEST_F(SnowballTest, Step2EntliToEnt) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("differentli"), "differ");
}

TEST_F(SnowballTest, Step2EliToE) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("vileli"), "vile");
}

TEST_F(SnowballTest, Step2OusliToOus) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("analogousli"), "analog");
}

TEST_F(SnowballTest, Step2IzationToIze) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("vietnamization"), "vietnam");
}

TEST_F(SnowballTest, Step2AtionToAte) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("predication"), "predic");
}

TEST_F(SnowballTest, Step2AtorToAte) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("operator"), "oper");
}

TEST_F(SnowballTest, Step2AlismToAl) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("feudalism"), "feudal");
}

TEST_F(SnowballTest, Step2IvenesstoIve) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("decisiveness"), "decis");
}

TEST_F(SnowballTest, Step2FulnesstoFul) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("hopefulness"), "hope");
}

TEST_F(SnowballTest, Step2OusnesstoOus) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("callousness"), "callous");
}

TEST_F(SnowballTest, Step2AlitiToAl) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("formaliti"), "formal");
}

TEST_F(SnowballTest, Step2IvitiToIve) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("sensitiviti"), "sensit");
}

TEST_F(SnowballTest, Step2BilitiToBle) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("sensibiliti"), "sensibl");
}

// ===========================================================================
// SnowballStemmer — Step 3 (m>0 suffix transforms)
// ===========================================================================

TEST_F(SnowballTest, Step3IcateToIc) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("triplicate"), "triplic");
}

TEST_F(SnowballTest, Step3AtiveRemoved) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("formative"), "form");
}

TEST_F(SnowballTest, Step3AlizeToAl) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("formalize"), "formal");
}

TEST_F(SnowballTest, Step3IcitiToIc) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("electriciti"), "electr");
}

TEST_F(SnowballTest, Step3IcalToIc) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("electrical"), "electr");
}

TEST_F(SnowballTest, Step3FulRemoved) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("hopeful"), "hope");
}

TEST_F(SnowballTest, Step3NessRemoved) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("goodness"), "good");
}

// ===========================================================================
// SnowballStemmer — Step 4 (m>1 suffix removal)
// ===========================================================================

TEST_F(SnowballTest, Step4AlRemoved) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("revival"), "reviv");
}

TEST_F(SnowballTest, Step4AnceRemoved) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("allowance"), "allow");
}

TEST_F(SnowballTest, Step4EnceRemoved) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("inference"), "infer");
}

TEST_F(SnowballTest, Step4ErRemoved) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("airliner"), "airlin");
}

TEST_F(SnowballTest, Step4IcRemoved) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("gyroscopic"), "gyroscop");
}

TEST_F(SnowballTest, Step4AbleRemoved) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("adjustable"), "adjust");
}

TEST_F(SnowballTest, Step4IbleRemoved) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("defensible"), "defens");
}

TEST_F(SnowballTest, Step4AntRemoved) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("irritant"), "irrit");
}

TEST_F(SnowballTest, Step4MentRemoved) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("adjustment"), "adjust");
}

TEST_F(SnowballTest, Step4EntRemoved) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("dependent"), "depend");
}

TEST_F(SnowballTest, Step4IonRemovedIfSorT) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("adoption"), "adopt");
    EXPECT_EQ(s.Stem("homologou"), "homolog");
}

TEST_F(SnowballTest, Step4IsmRemoved) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("communism"), "commun");
}

TEST_F(SnowballTest, Step4AteRemoved) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("activate"), "activ");
}

TEST_F(SnowballTest, Step4ItiRemoved) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("angulariti"), "angular");
}

TEST_F(SnowballTest, Step4OusRemoved) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("homologous"), "homolog");
}

TEST_F(SnowballTest, Step4IveRemoved) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("effective"), "effect");
}

TEST_F(SnowballTest, Step4IzeRemoved) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("bowdlerize"), "bowdler");
}

// ===========================================================================
// SnowballStemmer — Step 5a (final e)
// ===========================================================================

TEST_F(SnowballTest, Step5aRemoveEIfMgt1) {
    SnowballStemmer s;
    // "probate" -> m("prob")=1, not >1; EndsCvc? p(c)r(c)o(v)b(c) ->
    // b is consonant, o is vowel, b is consonant -> cvc! b not w/x/y -> *o.
    // (m=1 and *o) -> keep 'e'. Result "probate"
    EXPECT_EQ(s.Stem("probate"), "probat");
    // "rate" -> m("r")=0, m=1? no. Actually "rate": Ends "e", stem "rat",
    // m("rat")=1. EndsCvc("rat")? r(c)a(v)t(c) -> cvc, t not w/x/y -> *o.
    // (m=1 and *o) -> keep 'e' -> "rate". Then step5a doesn't remove.
    EXPECT_EQ(s.Stem("rate"), "rate");
}

TEST_F(SnowballTest, Step5aRemoveEIfM1NotCvc) {
    SnowballStemmer s;
    // "cease" -> Ends "e", stem "ceas". m("ceas")=1. EndsCvc? c(v... wait
    // c(c)e(v)a(v)s(c) -> not cvc (need CVC, but eas is VVC). *o=false.
    // (m=1 and not *o) -> remove 'e' -> "ceas"
    EXPECT_EQ(s.Stem("cease"), "ceas");
}

// ===========================================================================
// SnowballStemmer — Step 5b (double l)
// ===========================================================================

TEST_F(SnowballTest, Step5bDoubleLReduced) {
    SnowballStemmer s;
    // "controll" -> m("controll")=? c(c)o(v)n(c)t(c)r(c)o(v)l(c)l(c) ->
    // VC: o-n, o-l -> m=2. m>1, double l, last is 'l' -> remove one -> "control"
    EXPECT_EQ(s.Stem("controll"), "control");
}

TEST_F(SnowballTest, Step5bRollUnchanged) {
    SnowballStemmer s;
    // "roll" -> m("roll")=1 (o-ll -> one VC). m not >1, no reduction.
    EXPECT_EQ(s.Stem("roll"), "roll");
}

// ===========================================================================
// SnowballStemmer — edge cases
// ===========================================================================

TEST_F(SnowballTest, EmptyStringReturnsEmpty) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem(""), "");
}

TEST_F(SnowballTest, ShortWordsUnchanged) {
    SnowballStemmer s;
    // Words of length <= 2 are returned unchanged (Porter convention).
    EXPECT_EQ(s.Stem("a"), "a");
    EXPECT_EQ(s.Stem("ab"), "ab");
    EXPECT_EQ(s.Stem("is"), "is");
}

TEST_F(SnowballTest, UppercaseInputLowercased) {
    SnowballStemmer s;
    EXPECT_EQ(s.Stem("RUNNING"), "run");
    EXPECT_EQ(s.Stem("Cats"), "cat");
    EXPECT_EQ(s.Stem("AgReEd"), "agre");
}

TEST_F(SnowballTest, NonAlphaReturnedLowercased) {
    SnowballStemmer s;
    // Words with non-letter characters are returned lowercased unchanged.
    EXPECT_EQ(s.Stem("abc123"), "abc123");
    EXPECT_EQ(s.Stem("hello-world"), "hello-world");
}

TEST_F(SnowballTest, IdempotencyStemmingAlreadyStemmed) {
    SnowballStemmer s;
    // Stemming an already-stemmed word should produce the same stem.
    std::string stem1 = s.Stem("running");
    std::string stem2 = s.Stem(stem1);
    EXPECT_EQ(stem1, stem2);
    EXPECT_EQ(stem1, "run");
}

// ===========================================================================
// SnowballDict
// ===========================================================================

TEST_F(SnowballTest, SnowballDictStemsWord) {
    SnowballDict dict;
    Lexeme lex = dict.Lexicalize("running");
    EXPECT_EQ(lex.text, "run");
    EXPECT_FALSE(lex.is_stop);
}

TEST_F(SnowballTest, SnowballDictLowercasesInput) {
    SnowballDict dict;
    EXPECT_EQ(dict.Lexicalize("CATS").text, "cat");
    EXPECT_EQ(dict.Lexicalize("Happiness").text, "happi");
}

TEST_F(SnowballTest, SnowballDictWithStopWordsFilters) {
    SnowballDict dict({"the", "is", "a"});
    Lexeme stop = dict.Lexicalize("the");
    EXPECT_TRUE(stop.is_stop);
    EXPECT_EQ(stop.text, "the");
    Lexeme notstop = dict.Lexicalize("running");
    EXPECT_FALSE(notstop.is_stop);
    EXPECT_EQ(notstop.text, "run");
}

TEST_F(SnowballTest, SnowballDictStopWordsCaseInsensitive) {
    SnowballDict dict({"the"});
    EXPECT_TRUE(dict.Lexicalize("THE").is_stop);
    EXPECT_TRUE(dict.Lexicalize("The").is_stop);
}

TEST_F(SnowballTest, SnowballDictNoStopWordsByDefault) {
    SnowballDict dict;
    // Without a stop-word list, no word is filtered.
    EXPECT_FALSE(dict.Lexicalize("the").is_stop);
    EXPECT_FALSE(dict.Lexicalize("is").is_stop);
}

// ===========================================================================
// TsearchConfigRegistry
// ===========================================================================

TEST_F(SnowballTest, RegistryHasBuiltinSimpleConfig) {
    auto& reg = GetTsearchConfigRegistry();
    EXPECT_TRUE(reg.HasConfig("simple"));
}

TEST_F(SnowballTest, RegistryHasBuiltinEnglishConfig) {
    auto& reg = GetTsearchConfigRegistry();
    EXPECT_TRUE(reg.HasConfig("english"));
}

TEST_F(SnowballTest, RegistryUnknownConfigReturnsEmptyChain) {
    auto& reg = GetTsearchConfigRegistry();
    auto chain = reg.BuildChain("nonexistent");
    EXPECT_TRUE(chain.empty());
}

TEST_F(SnowballTest, RegistryUnknownConfigNotHasConfig) {
    auto& reg = GetTsearchConfigRegistry();
    EXPECT_FALSE(reg.HasConfig("nonexistent"));
}

TEST_F(SnowballTest, RegistrySimpleChainHasOneDict) {
    auto& reg = GetTsearchConfigRegistry();
    auto chain = reg.BuildChain("simple");
    ASSERT_EQ(chain.size(), 1u);
}

TEST_F(SnowballTest, RegistryEnglishChainHasTwoDicts) {
    auto& reg = GetTsearchConfigRegistry();
    auto chain = reg.BuildChain("english");
    ASSERT_EQ(chain.size(), 2u);
}

TEST_F(SnowballTest, RegistryCustomConfigRegistration) {
    TsearchConfigRegistry reg;
    reg.Register("custom", []() -> TsearchConfigRegistry::DictChain {
        TsearchConfigRegistry::DictChain chain;
        chain.push_back(std::make_unique<SnowballDict>());
        return chain;
    });
    EXPECT_TRUE(reg.HasConfig("custom"));
    auto chain = reg.BuildChain("custom");
    ASSERT_EQ(chain.size(), 1u);
    Lexeme lex = chain[0]->Lexicalize("running");
    EXPECT_EQ(lex.text, "run");
}

TEST_F(SnowballTest, RegistryRegisterReplacesExisting) {
    TsearchConfigRegistry reg;
    reg.Register("cfg", []() -> TsearchConfigRegistry::DictChain {
        TsearchConfigRegistry::DictChain chain;
        chain.push_back(std::make_unique<SnowballDict>());
        return chain;
    });
    // Replace with a 2-dictionary chain.
    reg.Register("cfg", []() -> TsearchConfigRegistry::DictChain {
        TsearchConfigRegistry::DictChain chain;
        chain.push_back(std::make_unique<SnowballDict>());
        chain.push_back(std::make_unique<SnowballDict>());
        return chain;
    });
    auto chain = reg.BuildChain("cfg");
    EXPECT_EQ(chain.size(), 2u);
}

TEST_F(SnowballTest, RegisterBuiltinConfigsIdempotent) {
    TsearchConfigRegistry reg;
    RegisterBuiltinTsearchConfigs(reg);
    EXPECT_TRUE(reg.HasConfig("simple"));
    EXPECT_TRUE(reg.HasConfig("english"));
    // Calling again should not duplicate entries.
    RegisterBuiltinTsearchConfigs(reg);
    auto simple_chain = reg.BuildChain("simple");
    EXPECT_EQ(simple_chain.size(), 1u);
    auto english_chain = reg.BuildChain("english");
    EXPECT_EQ(english_chain.size(), 2u);
}

// ===========================================================================
// ApplyDictionaryChain
// ===========================================================================

TEST_F(SnowballTest, ApplyChainSimpleConfigLowercases) {
    auto& reg = GetTsearchConfigRegistry();
    auto chain = reg.BuildChain("simple");
    auto lex = ApplyDictionaryChain(chain, "HELLO");
    ASSERT_TRUE(lex.has_value());
    EXPECT_EQ(lex->text, "hello");
    EXPECT_FALSE(lex->is_stop);
}

TEST_F(SnowballTest, ApplyChainEnglishConfigStemsAndFiltersStop) {
    auto& reg = GetTsearchConfigRegistry();
    auto chain = reg.BuildChain("english");
    // "the" is a stop word -> filtered.
    auto stop = ApplyDictionaryChain(chain, "the");
    EXPECT_FALSE(stop.has_value());
    // "running" -> stop check passes -> snowball stems -> "run".
    auto stem = ApplyDictionaryChain(chain, "running");
    ASSERT_TRUE(stem.has_value());
    EXPECT_EQ(stem->text, "run");
}

TEST_F(SnowballTest, ApplyChainEnglishConfigKeepsNonStop) {
    auto& reg = GetTsearchConfigRegistry();
    auto chain = reg.BuildChain("english");
    auto lex = ApplyDictionaryChain(chain, "database");
    ASSERT_TRUE(lex.has_value());
    EXPECT_EQ(lex->text, "databas");
}

TEST_F(SnowballTest, ApplyChainEmptyReturnsOriginal) {
    TsearchConfigRegistry::DictChain empty;
    auto lex = ApplyDictionaryChain(empty, "hello");
    ASSERT_TRUE(lex.has_value());
    EXPECT_EQ(lex->text, "hello");
    EXPECT_FALSE(lex->is_stop);
}

// ===========================================================================
// ToTsVector integration with snowball stemming
// ===========================================================================

TEST_F(SnowballTest, ToTsVectorEnglishUsesSnowballNotSimple) {
    auto vec = ToTsVector("running jumped quickly", "english");
    std::vector<std::string> lexemes;
    for (const auto& e : vec.entries) {
        lexemes.push_back(e.lexeme);
    }
    // Snowball produces real stems, not SimpleDict's naive stripping.
    EXPECT_NE(std::find(lexemes.begin(), lexemes.end(), "run"), lexemes.end());
    EXPECT_NE(std::find(lexemes.begin(), lexemes.end(), "jump"), lexemes.end());
    EXPECT_NE(std::find(lexemes.begin(), lexemes.end(), "quickli"), lexemes.end());
}

TEST_F(SnowballTest, ToTsVectorEnglishStemsConsistently) {
    // "running" and "runs" should both stem to "run".
    auto vec = ToTsVector("running runs", "english");
    ASSERT_EQ(vec.entries.size(), 1u);
    EXPECT_EQ(vec.entries[0].lexeme, "run");
    ASSERT_EQ(vec.entries[0].positions.size(), 2u);
}

TEST_F(SnowballTest, ToTsVectorSimpleConfigNoSnowballStemming) {
    auto vec = ToTsVector("running", "simple");
    // "simple" config uses SimpleDict (naive), not snowball.
    ASSERT_EQ(vec.entries.size(), 1u);
    // SimpleDict strips "ing" -> "runn".
    EXPECT_EQ(vec.entries[0].lexeme, "runn");
}

TEST_F(SnowballTest, ToTsVectorEnglishFiltersStopWords) {
    auto vec = ToTsVector("the cat is on the mat", "english");
    std::vector<std::string> lexemes;
    for (const auto& e : vec.entries) {
        lexemes.push_back(e.lexeme);
    }
    EXPECT_EQ(std::find(lexemes.begin(), lexemes.end(), "the"), lexemes.end());
    EXPECT_EQ(std::find(lexemes.begin(), lexemes.end(), "is"), lexemes.end());
    EXPECT_EQ(std::find(lexemes.begin(), lexemes.end(), "on"), lexemes.end());
}

// ===========================================================================
// ToTsQuery integration with snowball stemming
// ===========================================================================

TEST_F(SnowballTest, ToTsQueryEnglishStemsTerms) {
    auto q = ToTsQuery("running jumping", "english");
    EXPECT_EQ(q.root.type, pgcpp::types::TsQueryNodeType::kAnd);
    ASSERT_EQ(q.root.children.size(), 2u);
    EXPECT_EQ(q.root.children[0].lexeme, "run");
    EXPECT_EQ(q.root.children[1].lexeme, "jump");
}

TEST_F(SnowballTest, ToTsQueryEnglishFiltersStopWords) {
    auto q = ToTsQuery("the running", "english");
    // "the" filtered -> single term "run".
    EXPECT_EQ(q.root.type, pgcpp::types::TsQueryNodeType::kTerm);
    EXPECT_EQ(q.root.lexeme, "run");
}

// ===========================================================================
// Catalog row types (pg_ts_dict / pg_ts_cfg / pg_ts_template / pg_ts_parser)
// ===========================================================================

TEST_F(SnowballTest, PgTsDictDefaults) {
    FormData_pg_ts_dict row;
    EXPECT_EQ(row.oid, kInvalidOid);
    EXPECT_TRUE(row.dictname.empty());
    EXPECT_EQ(row.dictnamespace, kInvalidOid);
    EXPECT_EQ(row.dictowner, kInvalidOid);
    EXPECT_EQ(row.dicttemplate, kInvalidOid);
    EXPECT_TRUE(row.dictinitoption.empty());
}

TEST_F(SnowballTest, PgTsDictFieldAssignment) {
    FormData_pg_ts_dict row;
    row.oid = 4200;
    row.dictname = "english_stem";
    row.dicttemplate = 4300;
    row.dictinitoption = "StopWords=english";
    EXPECT_EQ(row.oid, 4200u);
    EXPECT_EQ(row.dictname, "english_stem");
    EXPECT_EQ(row.dicttemplate, 4300u);
    EXPECT_EQ(row.dictinitoption, "StopWords=english");
}

TEST_F(SnowballTest, PgTsCfgDefaults) {
    FormData_pg_ts_cfg row;
    EXPECT_EQ(row.oid, kInvalidOid);
    EXPECT_TRUE(row.cfgname.empty());
    EXPECT_EQ(row.cfgnamespace, kInvalidOid);
    EXPECT_EQ(row.cfgowner, kInvalidOid);
    EXPECT_EQ(row.cfgparser, kInvalidOid);
    EXPECT_TRUE(row.cfgmap.empty());
}

TEST_F(SnowballTest, PgTsCfgCfgMap) {
    FormData_pg_ts_cfg row;
    row.cfgname = "english";
    row.cfgmap[1] = "4200";  // token type 1 -> dict OID 4200
    row.cfgmap[2] = "4200,4201";
    EXPECT_EQ(row.cfgname, "english");
    ASSERT_EQ(row.cfgmap.size(), 2u);
    EXPECT_EQ(row.cfgmap[1], "4200");
    EXPECT_EQ(row.cfgmap[2], "4200,4201");
}

TEST_F(SnowballTest, PgTsTemplateDefaults) {
    FormData_pg_ts_template row;
    EXPECT_EQ(row.oid, kInvalidOid);
    EXPECT_TRUE(row.tmplname.empty());
    EXPECT_EQ(row.tmplnamespace, kInvalidOid);
    EXPECT_EQ(row.tmplinit, kInvalidOid);
    EXPECT_EQ(row.tmpllexize, kInvalidOid);
}

TEST_F(SnowballTest, PgTsParserDefaults) {
    FormData_pg_ts_parser row;
    EXPECT_EQ(row.oid, kInvalidOid);
    EXPECT_TRUE(row.prsname.empty());
    EXPECT_EQ(row.prsnamespace, kInvalidOid);
    EXPECT_EQ(row.prsstart, kInvalidOid);
    EXPECT_EQ(row.prstoken, kInvalidOid);
    EXPECT_EQ(row.prsend, kInvalidOid);
    EXPECT_EQ(row.prsheadline, kInvalidOid);
    EXPECT_EQ(row.prslextype, kInvalidOid);
}

}  // namespace

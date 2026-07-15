#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "tsearch/dict.hpp"

namespace pgcpp::tsearch {

// ---------------------------------------------------------------------------
// SnowballStemmer — Porter/Snowball English stemming algorithm
// (PostgreSQL src/backend/snowball/, stem_ISO_8859_1_english.sbl).
//
// Implements the classic Porter (1980) / Snowball English stemmer: a sequence
// of measure-based suffix transformation steps that reduce inflected English
// words to their stem. This replaces the naive suffix stripping of SimpleDict
// with a real algorithmic stemmer matching PostgreSQL's "english" text-search
// configuration behavior.
//
// The stemmer operates on lowercase ASCII a-z input. Non-letter characters
// terminate the region processed; callers should lowercase beforehand (the
// public API handles lowercasing).
// ---------------------------------------------------------------------------
class SnowballStemmer {
public:
    // Reduce `word` to its Snowball/Porter stem. The input is lowercased
    // internally. Returns the stem.
    std::string Stem(std::string_view word) const;
};

// SnowballDict — text-search dictionary backed by the Snowball stemmer
// (PostgreSQL's "snowball" dictionary template).
//
// Lexicalize lowercases the word, applies the Snowball stemmer, and returns
// the stem. Optionally filters stop words (when constructed with a stop-word
// list): stop words are returned with is_stop=true so the pipeline drops them.
class SnowballDict : public IDictionary {
public:
    SnowballDict() = default;

    // If `stop_words` is non-empty, words present in it (case-insensitive)
    // are filtered out before stemming.
    explicit SnowballDict(std::vector<std::string> stop_words)
        : stop_words_(std::move(stop_words)) {}

    Lexeme Lexicalize(std::string_view word) const override;

private:
    std::vector<std::string> stop_words_;
};

}  // namespace pgcpp::tsearch

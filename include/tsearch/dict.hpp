#pragma once

#include <string>
#include <string_view>

namespace pgcpp::tsearch {

// ---------------------------------------------------------------------------
// Dictionary interface (PostgreSQL src/backend/tsearch/dict.c).
//
// A dictionary transforms a raw token into a lexeme (canonical form) or
// discards it (e.g. stop words). This mirrors PG's TSDictionaryCacheEntry.
// ---------------------------------------------------------------------------

struct Lexeme {
    std::string text;      // The lemma / canonical form
    bool is_stop = false;  // If true, the word is filtered out
};

class IDictionary {
public:
    virtual ~IDictionary() = default;
    virtual Lexeme Lexicalize(std::string_view word) const = 0;
};

// SimpleDict — basic English stemming: lowercase, strip common suffixes
// ('s, 'ed, 'ing, 'es, 'ly). Mirrors PG's "simple" dictionary template.
class SimpleDict : public IDictionary {
public:
    Lexeme Lexicalize(std::string_view word) const override;
};

// StopWordsDict — filters out common English stop words. Non-stop words
// pass through unchanged (caller is responsible for further normalization).
class StopWordsDict : public IDictionary {
public:
    Lexeme Lexicalize(std::string_view word) const override;
};

}  // namespace pgcpp::tsearch

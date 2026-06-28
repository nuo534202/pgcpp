#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "pgcpp/tsearch/dict.hpp"

namespace mytoydb::tsearch {

// ---------------------------------------------------------------------------
// Ispell affix dictionary (PostgreSQL src/backend/tsearch/spell.c).
//
// Simplified: stores a dictionary of stem words and a list of prefix/suffix
// affixes. Lexicalize tries to strip a known affix from the input and check
// whether the remainder is a known stem; if so the stem is returned.
// ---------------------------------------------------------------------------

class IspellDict : public IDictionary {
public:
    // Register a stem word (already lowercased).
    void AddWord(std::string word);

    // Register an affix. `is_prefix` distinguishes prefixes from suffixes.
    // `affix` is the literal string to strip (already lowercased).
    void AddAffix(std::string affix, bool is_prefix);

    Lexeme Lexicalize(std::string_view word) const override;

    std::size_t WordCount() const { return words_.size(); }
    std::size_t AffixCount() const { return affixes_.size(); }

private:
    struct Affix {
        std::string text;
        bool is_prefix;
    };

    std::vector<std::string> words_;
    std::vector<Affix> affixes_;
};

}  // namespace mytoydb::tsearch

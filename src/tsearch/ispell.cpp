// ispell.cpp — simplified Ispell affix dictionary.
//
// Lexicalize: lowercase the word, then try stripping each known affix and
// checking whether the remainder matches a stored stem. If a stem is found,
// return it; otherwise return the lowercased word unchanged.

#include "tsearch/ispell.hpp"

#include <algorithm>
#include <cctype>

namespace pgcpp::tsearch {

namespace {

std::string ToLower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

}  // namespace

void IspellDict::AddWord(std::string word) {
    std::transform(word.begin(), word.end(), word.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    words_.push_back(std::move(word));
}

void IspellDict::AddAffix(std::string affix, bool is_prefix) {
    std::transform(affix.begin(), affix.end(), affix.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    affixes_.push_back({std::move(affix), is_prefix});
}

Lexeme IspellDict::Lexicalize(std::string_view word) const {
    Lexeme lex;
    lex.text = ToLower(word);
    for (const auto& affix : affixes_) {
        if (affix.is_prefix) {
            if (lex.text.size() > affix.text.size() &&
                lex.text.compare(0, affix.text.size(), affix.text) == 0) {
                std::string stem = lex.text.substr(affix.text.size());
                if (std::find(words_.begin(), words_.end(), stem) != words_.end()) {
                    lex.text = std::move(stem);
                    return lex;
                }
            }
        } else {
            if (lex.text.size() > affix.text.size() &&
                lex.text.compare(lex.text.size() - affix.text.size(), affix.text.size(),
                                 affix.text) == 0) {
                std::string stem = lex.text.substr(0, lex.text.size() - affix.text.size());
                if (std::find(words_.begin(), words_.end(), stem) != words_.end()) {
                    lex.text = std::move(stem);
                    return lex;
                }
            }
        }
    }
    return lex;
}

}  // namespace pgcpp::tsearch

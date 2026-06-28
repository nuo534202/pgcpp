// dict.cpp — dictionary implementations.
//
// SimpleDict performs a small set of English suffix-stripping rules.
// StopWordsDict returns is_stop=true for common English function words.

#include "mytoydb/tsearch/dict.hpp"

#include <array>
#include <cctype>
#include <cstring>
#include <string_view>

namespace mytoydb::tsearch {

namespace {

std::string ToLower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

// Strip `suffix` (case-insensitive) from `word` if present. Returns true and
// truncates `word` in place; returns false otherwise.
bool StripSuffix(std::string& word, std::string_view suffix) {
    if (word.size() <= suffix.size()) {
        return false;
    }
    std::size_t start = word.size() - suffix.size();
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(word[start + i])) !=
            std::tolower(static_cast<unsigned char>(suffix[i]))) {
            return false;
        }
    }
    word.resize(start);
    return true;
}

constexpr std::string_view kStopWords[] = {
    "a",   "an",    "and",  "are",   "as",    "at",   "be",   "but", "by",  "for",  "if",
    "in",  "into",  "is",   "it",    "no",    "not",  "of",   "on",  "or",  "such", "that",
    "the", "their", "then", "there", "these", "they", "this", "to",  "was", "will", "with",
    "i",   "you",   "he",   "she",   "we",    "his",  "her",  "its",
};

bool IsStopWord(std::string_view word) {
    for (auto sw : kStopWords) {
        if (word == sw) {
            return true;
        }
    }
    return false;
}

}  // namespace

Lexeme SimpleDict::Lexicalize(std::string_view word) const {
    Lexeme lex;
    lex.text = ToLower(word);
    // Order matters: longer suffixes first so "ies" -> "y" beats "s".
    if (StripSuffix(lex.text, "ies")) {
        lex.text.push_back('y');
    } else if (StripSuffix(lex.text, "es")) {
        // keep stem
    } else if (StripSuffix(lex.text, "s")) {
        // keep stem
    } else if (StripSuffix(lex.text, "ed")) {
        // keep stem
    } else if (StripSuffix(lex.text, "ing")) {
        // keep stem
    } else if (StripSuffix(lex.text, "ly")) {
        // keep stem
    }
    return lex;
}

Lexeme StopWordsDict::Lexicalize(std::string_view word) const {
    Lexeme lex;
    lex.text = ToLower(word);
    lex.is_stop = IsStopWord(lex.text);
    return lex;
}

}  // namespace mytoydb::tsearch

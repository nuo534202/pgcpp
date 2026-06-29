#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pgcpp::tsearch {

// ---------------------------------------------------------------------------
// Word parser / tokenizer (PostgreSQL src/backend/tsearch/wparser.c).
//
// Splits input text into tokens based on whitespace and punctuation.
// Each token records its 1-based position in the source text and an optional
// weight ('A'..'D', 'D' being the default).
// ---------------------------------------------------------------------------

struct Token {
    std::string text;
    int32_t position = 0;  // 1-based position in the source text
    char weight = 'D';     // PostgreSQL weight: A (highest) .. D (default)
};

// Tokenize text into a list of tokens. Whitespace and punctuation are treated
// as token separators. Tokens are lowercased to match PostgreSQL's default
// tokenization behavior (which is configuration-dependent, but the common
// "simple" config lowercases).
std::vector<Token> TokenizeText(std::string_view str);

}  // namespace pgcpp::tsearch

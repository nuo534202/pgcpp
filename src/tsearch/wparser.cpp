// wparser.cpp — word parser / tokenizer.
//
// Splits text into alphanumeric tokens. Punctuation and whitespace are
// treated as separators. Tokens are lowercased so that downstream
// dictionaries see canonical input.

#include "tsearch/wparser.hpp"

#include <cctype>

namespace pgcpp::tsearch {

namespace {

bool IsTokenChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

char ToLower(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

}  // namespace

std::vector<Token> TokenizeText(std::string_view str) {
    std::vector<Token> tokens;
    int32_t position = 0;
    std::size_t i = 0;
    while (i < str.size()) {
        // Skip separators.
        while (i < str.size() && !IsTokenChar(str[i])) {
            ++i;
        }
        if (i >= str.size()) {
            break;
        }
        std::string text;
        while (i < str.size() && IsTokenChar(str[i])) {
            text.push_back(ToLower(str[i]));
            ++i;
        }
        Token tok;
        tok.text = std::move(text);
        tok.position = ++position;
        tok.weight = 'D';
        tokens.push_back(std::move(tok));
    }
    return tokens;
}

}  // namespace pgcpp::tsearch

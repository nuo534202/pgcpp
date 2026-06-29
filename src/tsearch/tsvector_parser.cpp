// tsvector_parser.cpp — parse "lexeme:posA,posB lexeme2:posC" literals.
//
// Grammar (simplified from PG):
//   tsvector    := entry (ws entry)*
//   entry       := lexeme (':' positions)?
//   lexeme      := [A-Za-z0-9_]+
//   positions   := pos_entry (',' pos_entry)*
//   pos_entry   := digit+ weight?
//   weight      := 'A' | 'B' | 'C' | 'D'

#include "tsearch/tsvector_parser.hpp"

#include <cctype>

#include "common/error/elog.hpp"

namespace pgcpp::tsearch {

using pgcpp::error::LogLevel;

namespace {

bool IsLexemeChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool IsWeightChar(char c) {
    return c == 'A' || c == 'B' || c == 'C' || c == 'D';
}

}  // namespace

std::vector<WordEntry> TsVectorParse(std::string_view str) {
    std::vector<WordEntry> result;
    std::size_t i = 0;
    while (i < str.size()) {
        // Skip whitespace between entries.
        while (i < str.size() && std::isspace(static_cast<unsigned char>(str[i]))) {
            ++i;
        }
        if (i >= str.size()) {
            break;
        }
        // Read lexeme.
        std::string lexeme;
        while (i < str.size() && IsLexemeChar(str[i])) {
            lexeme.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(str[i]))));
            ++i;
        }
        if (lexeme.empty()) {
            ereport(
                LogLevel::kError,
                "invalid tsvector literal: unexpected character at position " + std::to_string(i));
        }
        WordEntry entry;
        entry.lexeme = std::move(lexeme);
        // Optional ":positions" suffix.
        if (i < str.size() && str[i] == ':') {
            ++i;
            while (i < str.size()) {
                // Parse position number.
                if (i >= str.size() || !std::isdigit(static_cast<unsigned char>(str[i]))) {
                    ereport(LogLevel::kError,
                            "invalid tsvector literal: expected digit at position " +
                                std::to_string(i));
                }
                int32_t pos = 0;
                while (i < str.size() && std::isdigit(static_cast<unsigned char>(str[i]))) {
                    pos = pos * 10 + (str[i] - '0');
                    ++i;
                }
                entry.positions.push_back(pos);
                // Optional weight immediately after the digit.
                if (i < str.size() && IsWeightChar(str[i])) {
                    entry.weights.push_back(str[i]);
                    ++i;
                } else {
                    entry.weights.push_back('D');
                }
                // Comma → another position entry; otherwise we're done.
                if (i < str.size() && str[i] == ',') {
                    ++i;
                    continue;
                }
                break;
            }
        }
        result.push_back(std::move(entry));
    }
    return result;
}

}  // namespace pgcpp::tsearch

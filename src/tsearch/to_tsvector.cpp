// to_tsvector.cpp — text → tsvector pipeline.
//
// 1. Tokenize the input text (TokenizeText).
// 2. Apply a per-config dictionary chain:
//    - "simple": lowercase only, keep all tokens.
//    - "english" (default): lowercase + SimpleDict stemming + stop-word removal.
// 3. Build a TsVectorData with deduplicated lexemes and 1-based positions,
//    sorted ascending by lexeme (matching PG's on-disk format).

#include "pgcpp/tsearch/to_tsvector.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "pgcpp/tsearch/dict.hpp"
#include "pgcpp/tsearch/wparser.hpp"
#include "pgcpp/types/ts_types.hpp"

namespace pgcpp::tsearch {

using pgcpp::types::TsVectorData;
using pgcpp::types::TsWordEntry;

namespace {

// Run the dictionary chain appropriate for `config`. Returns the surviving
// lexemes with their original token positions.
std::map<std::string, std::vector<int32_t>> BuildLexemeMap(const std::vector<Token>& tokens,
                                                           std::string_view config) {
    std::map<std::string, std::vector<int32_t>> lexemes;
    bool drop_stop = (config == "english");
    SimpleDict stemmer;
    StopWordsDict stopper;
    for (const Token& tok : tokens) {
        if (drop_stop) {
            Lexeme stop_check = stopper.Lexicalize(tok.text);
            if (stop_check.is_stop) {
                continue;
            }
        }
        Lexeme lex = stemmer.Lexicalize(tok.text);
        lexemes[lex.text].push_back(tok.position);
    }
    return lexemes;
}

}  // namespace

TsVectorData ToTsVector(std::string_view text, std::string_view config) {
    std::vector<Token> tokens = TokenizeText(text);
    auto lexemes = BuildLexemeMap(tokens, config);
    TsVectorData out;
    out.entries.reserve(lexemes.size());
    for (auto& [lexeme, positions] : lexemes) {
        TsWordEntry entry;
        entry.lexeme = lexeme;
        entry.positions = std::move(positions);
        out.entries.push_back(std::move(entry));
    }
    // The map already produces ascending lexeme order; sort to be explicit.
    std::sort(out.entries.begin(), out.entries.end(),
              [](const TsWordEntry& a, const TsWordEntry& b) { return a.lexeme < b.lexeme; });
    return out;
}

}  // namespace pgcpp::tsearch

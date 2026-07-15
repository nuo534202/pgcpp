// to_tsvector.cpp — text → tsvector pipeline.
//
// 1. Tokenize the input text (TokenizeText).
// 2. Build the dictionary chain for `config` via TsearchConfigRegistry and
//    apply it to each token (stop words are dropped; surviving lexemes are
//    transformed by the chain).
// 3. Build a TsVectorData with deduplicated lexemes and 1-based positions,
//    sorted ascending by lexeme (matching PG's on-disk format).

#include "tsearch/to_tsvector.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "tsearch/ts_config.hpp"
#include "tsearch/wparser.hpp"
#include "types/ts_types.hpp"

namespace pgcpp::tsearch {

using pgcpp::types::TsVectorData;
using pgcpp::types::TsWordEntry;

namespace {

// Run the dictionary chain for `config` over the tokens. Returns the surviving
// lexemes with their original token positions.
std::map<std::string, std::vector<int32_t>> BuildLexemeMap(const std::vector<Token>& tokens,
                                                           std::string_view config) {
    std::map<std::string, std::vector<int32_t>> lexemes;
    auto chain = GetTsearchConfigRegistry().BuildChain(config);
    for (const Token& tok : tokens) {
        auto lexeme = ApplyDictionaryChain(chain, tok.text);
        if (!lexeme.has_value()) {
            continue;  // stop word
        }
        lexemes[lexeme->text].push_back(tok.position);
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

// to_tsquery.cpp — text → tsquery pipeline.
//
// 1. Tokenize the input text (TokenizeText).
// 2. Apply the per-config dictionary chain (same as ToTsVector).
// 3. AND every surviving lexeme together into a single TsQueryData tree.
//
// The result mirrors PG's to_tsquery() when called with a list of words
// separated by spaces — the implicit operator is AND.

#include "mytoydb/tsearch/to_tsquery.hpp"

#include <string>
#include <vector>

#include "mytoydb/tsearch/dict.hpp"
#include "mytoydb/tsearch/wparser.hpp"
#include "mytoydb/types/ts_types.hpp"

namespace mytoydb::tsearch {

using mytoydb::types::TsQueryData;
using mytoydb::types::TsQueryNode;
using mytoydb::types::TsQueryNodeType;

namespace {

std::vector<std::string> LexicalizeTokens(const std::vector<Token>& tokens,
                                          std::string_view config) {
    std::vector<std::string> out;
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
        out.push_back(std::move(lex.text));
    }
    return out;
}

}  // namespace

TsQueryData ToTsQuery(std::string_view text, std::string_view config) {
    std::vector<Token> tokens = TokenizeText(text);
    auto lexemes = LexicalizeTokens(tokens, config);
    TsQueryData out;
    if (lexemes.empty()) {
        // Empty query: a single term matching the empty string.
        out.root.type = TsQueryNodeType::kTerm;
        return out;
    }
    if (lexemes.size() == 1) {
        out.root.type = TsQueryNodeType::kTerm;
        out.root.lexeme = std::move(lexemes[0]);
        return out;
    }
    out.root.type = TsQueryNodeType::kAnd;
    out.root.children.reserve(lexemes.size());
    for (auto& lex : lexemes) {
        TsQueryNode term;
        term.type = TsQueryNodeType::kTerm;
        term.lexeme = std::move(lex);
        out.root.children.push_back(std::move(term));
    }
    return out;
}

}  // namespace mytoydb::tsearch

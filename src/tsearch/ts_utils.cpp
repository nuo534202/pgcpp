// ts_utils.cpp — ranking, headline extraction, query rewrite.
//
// ts_rank   — frequency-weight sum over matched lexemes.
// ts_rank_cd — cover density: rewards lexemes that appear close together.
// ts_headline — extract a window of words around the first match.
// ts_rewrite  — substitute leaf lexemes with replacement subtrees.

#include "tsearch/ts_utils.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <vector>

namespace pgcpp::tsearch {

using pgcpp::types::TsQueryData;
using pgcpp::types::TsQueryNode;
using pgcpp::types::TsQueryNodeType;
using pgcpp::types::TsVectorData;
using pgcpp::types::TsWordEntry;

namespace {

// Weight → numeric contribution. PG: A=1.0 (when default normalization), but
// for simplicity we use A=4, B=3, C=2, D=1 weights baked into the entries.
float WeightValue(char w) {
    switch (w) {
        case 'A':
            return 4.0f;
        case 'B':
            return 3.0f;
        case 'C':
            return 2.0f;
        case 'D':
            return 1.0f;
        default:
            return 1.0f;
    }
}

// Walk the query tree collecting every term lexeme into `out`.
void CollectTerms(const TsQueryNode& node, std::set<std::string>& out) {
    switch (node.type) {
        case TsQueryNodeType::kTerm:
            out.insert(node.lexeme);
            return;
        case TsQueryNodeType::kNot:
            // Skip NOT children — their match should not contribute to rank.
            return;
        case TsQueryNodeType::kAnd:
        case TsQueryNodeType::kOr:
            for (const auto& c : node.children) {
                CollectTerms(c, out);
            }
            return;
    }
}

}  // namespace

float ts_rank(const TsVectorData& vec, const TsQueryData& query) {
    std::set<std::string> query_terms;
    CollectTerms(query.root, query_terms);
    if (query_terms.empty()) {
        return 0.0f;
    }
    float score = 0.0f;
    for (const TsWordEntry& entry : vec.entries) {
        if (query_terms.count(entry.lexeme) == 0) {
            continue;
        }
        // Each occurrence contributes its weight value. If the vector lacks
        // positions, count a single occurrence for the matched lexeme.
        float occurrences =
            entry.positions.empty() ? 1.0f : static_cast<float>(entry.positions.size());
        score += occurrences * WeightValue('D');
    }
    return score;
}

float ts_rank_cd(const TsVectorData& vec, const TsQueryData& query) {
    std::set<std::string> query_terms;
    CollectTerms(query.root, query_terms);
    if (query_terms.empty()) {
        return 0.0f;
    }
    // Gather matched (position) pairs in document order.
    struct Match {
        int32_t position;
    };
    std::vector<Match> matches;
    for (const TsWordEntry& entry : vec.entries) {
        if (query_terms.count(entry.lexeme) == 0) {
            continue;
        }
        for (int32_t pos : entry.positions) {
            matches.push_back({pos});
        }
    }
    if (matches.empty()) {
        // Even if the vector has no positions, count a hit per matched lexeme.
        std::size_t hits = 0;
        for (const TsWordEntry& entry : vec.entries) {
            if (query_terms.count(entry.lexeme) > 0) {
                ++hits;
            }
        }
        return static_cast<float>(hits);
    }
    std::sort(matches.begin(), matches.end(),
              [](const Match& a, const Match& b) { return a.position < b.position; });
    // Cover density: lower average gap between consecutive matches → higher score.
    float total_gap = 0.0f;
    for (std::size_t i = 1; i < matches.size(); ++i) {
        total_gap += static_cast<float>(matches[i].position - matches[i - 1].position);
    }
    float avg_gap = matches.size() > 1 ? total_gap / static_cast<float>(matches.size() - 1) : 1.0f;
    // More matches → higher score; larger gaps → lower score.
    float density = 1.0f / (1.0f + avg_gap);
    return static_cast<float>(matches.size()) * density;
}

std::string ts_headline(std::string_view text, const TsQueryData& query, std::size_t max_words,
                        std::string_view start_sel, std::string_view stop_sel) {
    std::set<std::string> query_terms;
    CollectTerms(query.root, query_terms);
    if (query_terms.empty()) {
        return std::string(text);
    }
    // Split the text into word / separator tokens, tracking word indices.
    struct Span {
        std::size_t start;
        std::size_t end;
        bool is_word;
    };
    std::vector<Span> spans;
    std::size_t i = 0;
    while (i < text.size()) {
        if (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_') {
            std::size_t start = i;
            while (i < text.size() &&
                   (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_')) {
                ++i;
            }
            spans.push_back({start, i, true});
        } else {
            std::size_t start = i;
            while (i < text.size() &&
                   !(std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_')) {
                ++i;
            }
            spans.push_back({start, i, false});
        }
    }
    // Find the first word span whose lowercased text matches a query term.
    std::size_t match_span = spans.size();
    for (std::size_t s = 0; s < spans.size(); ++s) {
        if (!spans[s].is_word)
            continue;
        std::string word(text.substr(spans[s].start, spans[s].end - spans[s].start));
        std::string lower;
        lower.reserve(word.size());
        for (char c : word) {
            lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        if (query_terms.count(lower) > 0) {
            match_span = s;
            break;
        }
    }
    if (match_span == spans.size()) {
        // No match — return first max_words worth of text.
        std::size_t end = 0;
        std::size_t words = 0;
        for (std::size_t s = 0; s < spans.size() && words < max_words; ++s) {
            end = spans[s].end;
            if (spans[s].is_word)
                ++words;
        }
        return std::string(text.substr(0, end));
    }
    // Center the window around the match: include up to max_words words.
    std::size_t half = max_words / 2;
    std::size_t start_word_index = (match_span > half) ? (match_span - half) : 0;
    // Walk forward max_words words from start_word_index.
    std::size_t end_span = start_word_index;
    std::size_t words_emitted = 0;
    for (std::size_t s = start_word_index; s < spans.size() && words_emitted < max_words; ++s) {
        end_span = s + 1;
        if (spans[s].is_word)
            ++words_emitted;
    }
    std::string out;
    std::string start_str(start_sel);
    std::string stop_str(stop_sel);
    for (std::size_t s = start_word_index; s < end_span; ++s) {
        std::string piece(text.substr(spans[s].start, spans[s].end - spans[s].start));
        if (spans[s].is_word) {
            std::string lower;
            lower.reserve(piece.size());
            for (char c : piece) {
                lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
            if (query_terms.count(lower) > 0) {
                out += start_str;
                out += piece;
                out += stop_str;
            } else {
                out += piece;
            }
        } else {
            out += piece;
        }
    }
    return out;
}

TsQueryNode ts_rewrite(const TsQueryNode& query, const std::vector<RewriteRule>& rules) {
    if (query.type == TsQueryNodeType::kTerm) {
        for (const auto& rule : rules) {
            if (query.lexeme == rule.target_lexeme) {
                return rule.replacement;
            }
        }
        return query;
    }
    TsQueryNode out;
    out.type = query.type;
    out.lexeme = query.lexeme;
    out.children.reserve(query.children.size());
    for (const auto& child : query.children) {
        out.children.push_back(ts_rewrite(child, rules));
    }
    return out;
}

}  // namespace pgcpp::tsearch

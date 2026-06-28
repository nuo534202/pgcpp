// tsquery_parser.cpp — recursive-descent parser for tsquery literals.
//
// Grammar (precedence: ! > & > |):
//   or_expr  := and_expr ('|' and_expr)*
//   and_expr := not_expr ('&' not_expr)*
//   not_expr := '!' not_expr | atom
//   atom     := '(' or_expr ')' | term
//   term     := [A-Za-z0-9_]+
//
// The parser produces a TsQueryNode tree. AND/OR nodes always have ≥2
// children (left-associative chain folded into a single node). NOT nodes
// have exactly one child.

#include "pgcpp/tsearch/tsquery_parser.hpp"

#include <cctype>
#include <string>

#include "pgcpp/common/error/elog.hpp"

namespace mytoydb::tsearch {

using mytoydb::error::LogLevel;
using mytoydb::types::TsQueryNode;
using mytoydb::types::TsQueryNodeType;

namespace {

class Parser {
public:
    explicit Parser(std::string_view str) : str_(str), pos_(0) {}

    TsQueryNode Parse() {
        SkipWs();
        TsQueryNode node = ParseOr();
        SkipWs();
        if (pos_ != str_.size()) {
            ereport(LogLevel::kError,
                    "invalid tsquery literal: trailing input at position " + std::to_string(pos_));
        }
        return node;
    }

private:
    TsQueryNode ParseOr() {
        TsQueryNode left = ParseAnd();
        SkipWs();
        bool seen_or = false;
        while (pos_ < str_.size() && str_[pos_] == '|') {
            seen_or = true;
            ++pos_;
            TsQueryNode right = ParseAnd();
            // Fold into a single OR node.
            if (left.type == TsQueryNodeType::kOr) {
                left.children.push_back(std::move(right));
            } else {
                TsQueryNode parent;
                parent.type = TsQueryNodeType::kOr;
                parent.children.push_back(std::move(left));
                parent.children.push_back(std::move(right));
                left = std::move(parent);
            }
            SkipWs();
        }
        if (!seen_or) {
            return left;
        }
        return left;
    }

    TsQueryNode ParseAnd() {
        TsQueryNode left = ParseNot();
        SkipWs();
        bool seen_and = false;
        while (pos_ < str_.size() && str_[pos_] == '&') {
            seen_and = true;
            ++pos_;
            TsQueryNode right = ParseNot();
            if (left.type == TsQueryNodeType::kAnd) {
                left.children.push_back(std::move(right));
            } else {
                TsQueryNode parent;
                parent.type = TsQueryNodeType::kAnd;
                parent.children.push_back(std::move(left));
                parent.children.push_back(std::move(right));
                left = std::move(parent);
            }
            SkipWs();
        }
        if (!seen_and) {
            return left;
        }
        return left;
    }

    TsQueryNode ParseNot() {
        SkipWs();
        if (pos_ < str_.size() && str_[pos_] == '!') {
            ++pos_;
            TsQueryNode child = ParseNot();
            TsQueryNode parent;
            parent.type = TsQueryNodeType::kNot;
            parent.children.push_back(std::move(child));
            return parent;
        }
        return ParseAtom();
    }

    TsQueryNode ParseAtom() {
        SkipWs();
        if (pos_ >= str_.size()) {
            ereport(LogLevel::kError, "invalid tsquery literal: unexpected end of input");
        }
        if (str_[pos_] == '(') {
            ++pos_;
            TsQueryNode inner = ParseOr();
            SkipWs();
            if (pos_ >= str_.size() || str_[pos_] != ')') {
                ereport(LogLevel::kError, "invalid tsquery literal: expected ')' at position " +
                                              std::to_string(pos_));
            }
            ++pos_;
            return inner;
        }
        // Parse a term: [A-Za-z0-9_]+
        std::string lexeme;
        while (pos_ < str_.size() &&
               (std::isalnum(static_cast<unsigned char>(str_[pos_])) || str_[pos_] == '_')) {
            lexeme.push_back(
                static_cast<char>(std::tolower(static_cast<unsigned char>(str_[pos_]))));
            ++pos_;
        }
        if (lexeme.empty()) {
            ereport(LogLevel::kError,
                    "invalid tsquery literal: expected term at position " + std::to_string(pos_));
        }
        TsQueryNode term;
        term.type = TsQueryNodeType::kTerm;
        term.lexeme = std::move(lexeme);
        return term;
    }

    void SkipWs() {
        while (pos_ < str_.size() && std::isspace(static_cast<unsigned char>(str_[pos_]))) {
            ++pos_;
        }
    }

    std::string_view str_;
    std::size_t pos_;
};

}  // namespace

TsQueryNode TsQueryParse(std::string_view str) {
    Parser p(str);
    return p.Parse();
}

}  // namespace mytoydb::tsearch

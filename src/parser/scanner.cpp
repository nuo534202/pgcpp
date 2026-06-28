// scanner.cpp — Hand-written SQL scanner for MyToyDB.
//
// Converted from PostgreSQL 15's src/backend/parser/scan.l.
// Instead of using Flex, the scanner is hand-written for clarity and
// to avoid the complexity of the Flex C++ skeleton. The scanner reads
// from ParserDriver::scanbuf and advances ParserDriver::scanpos.

#include <cctype>
#include <string>

#include "gram.tab.hpp"
#include "pgcpp/parser/keywords.hpp"
#include "pgcpp/parser/parser_driver.hpp"

namespace mytoydb_parser {

// Forward declaration — implemented below.
BisonParser::symbol_type yylex(ParserDriver& driver);

}  // namespace mytoydb_parser

using namespace mytoydb_parser;
using namespace mytoydb::parser;

// yylex — return the next token from the scan buffer.
// This free function is called by the Bison-generated parser via
// `yylex(driver)`. It lives in the mytoydb_parser namespace so that
// the call inside BisonParser::parse() resolves correctly.
BisonParser::symbol_type mytoydb_parser::yylex(ParserDriver& driver) {
    const std::string& s = driver.scanbuf;
    size_t& pos = driver.scanpos;

    // Skip whitespace and comments.
    while (pos < s.size()) {
        // Skip whitespace.
        if (std::isspace(static_cast<unsigned char>(s[pos]))) {
            pos++;
            continue;
        }
        // Skip line comments (-- ...).
        if (pos + 1 < s.size() && s[pos] == '-' && s[pos + 1] == '-') {
            while (pos < s.size() && s[pos] != '\n')
                pos++;
            continue;
        }
        // Skip block comments (/* ... */ with nesting).
        if (pos + 1 < s.size() && s[pos] == '/' && s[pos + 1] == '*') {
            int depth = 1;
            pos += 2;
            while (pos < s.size() && depth > 0) {
                if (pos + 1 < s.size() && s[pos] == '/' && s[pos + 1] == '*') {
                    depth++;
                    pos += 2;
                } else if (pos + 1 < s.size() && s[pos] == '*' && s[pos + 1] == '/') {
                    depth--;
                    pos += 2;
                } else {
                    pos++;
                }
            }
            continue;
        }
        break;
    }

    if (pos >= s.size()) {
        return BisonParser::make_YYEOF(static_cast<int>(pos));
    }

    int tok_start = static_cast<int>(pos);
    char c = s[pos];

    // Identifiers and keywords: [a-zA-Z_][a-zA-Z0-9_$]*
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
        size_t start = pos;
        while (pos < s.size() && (std::isalnum(static_cast<unsigned char>(s[pos])) ||
                                  s[pos] == '_' || s[pos] == '$')) {
            pos++;
        }
        std::string word(s, start, pos - start);
        // Lowercase for keyword lookup.
        std::string lower_word = word;
        for (auto& ch : lower_word)
            ch = std::tolower(static_cast<unsigned char>(ch));

        const KeywordEntry* kw = ScanKeywordLookup(lower_word);
        if (kw != nullptr) {
            // It's a keyword — keywords have no semantic value.
            return BisonParser::symbol_type(kw->token, tok_start);
        }
        // It's an identifier.
        return BisonParser::make_IDENT(word, tok_start);
    }

    // Quoted identifiers: "..."
    if (c == '"') {
        pos++;  // skip opening quote
        std::string result;
        while (pos < s.size()) {
            if (s[pos] == '"') {
                if (pos + 1 < s.size() && s[pos + 1] == '"') {
                    result += '"';
                    pos += 2;
                } else {
                    pos++;  // skip closing quote
                    break;
                }
            } else {
                result += s[pos++];
            }
        }
        return BisonParser::make_UIDENT(result, tok_start);
    }

    // Integer and float constants: [0-9]+ (with optional . or exponent)
    if (std::isdigit(static_cast<unsigned char>(c))) {
        size_t start = pos;
        while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos])))
            pos++;
        // Check for float (decimal point or exponent).
        if (pos < s.size() && s[pos] == '.') {
            pos++;
            while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos])))
                pos++;
            if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
                pos++;
                if (pos < s.size() && (s[pos] == '+' || s[pos] == '-'))
                    pos++;
                while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos])))
                    pos++;
            }
            std::string num(s, start, pos - start);
            return BisonParser::make_FCONST(num, tok_start);
        }
        if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
            pos++;
            if (pos < s.size() && (s[pos] == '+' || s[pos] == '-'))
                pos++;
            while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos])))
                pos++;
            std::string num(s, start, pos - start);
            return BisonParser::make_FCONST(num, tok_start);
        }
        std::string num(s, start, pos - start);
        int64_t val = std::atoll(num.c_str());
        return BisonParser::make_ICONST(val, tok_start);
    }

    // String constants: '...'
    if (c == '\'') {
        pos++;  // skip opening quote
        std::string result;
        while (pos < s.size()) {
            if (s[pos] == '\'') {
                if (pos + 1 < s.size() && s[pos + 1] == '\'') {
                    result += '\'';
                    pos += 2;
                } else {
                    pos++;  // skip closing quote
                    break;
                }
            } else {
                result += s[pos++];
            }
        }
        return BisonParser::make_SCONST(result, tok_start);
    }

    // Dollar-quoted strings ($tag$...$tag$) and parameter references ($1, $2, ...).
    if (c == '$') {
        // Parameter reference: $ followed by digits.
        if (pos + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[pos + 1]))) {
            pos++;  // skip $
            size_t start = pos;
            while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos])))
                pos++;
            std::string num(s, start, pos - start);
            int param_num = std::stoi(num);
            return BisonParser::make_PARAM(param_num, tok_start);
        }

        // Dollar-quoted string: $tag$...$tag$
        size_t tag_start = pos;
        pos++;  // skip opening $
        while (pos < s.size() &&
               (std::isalnum(static_cast<unsigned char>(s[pos])) || s[pos] == '_')) {
            pos++;
        }
        if (pos < s.size() && s[pos] == '$') {
            std::string tag(s, tag_start, pos - tag_start + 1);  // includes both $
            pos++;                                               // skip closing $ of opening tag
            size_t content_start = pos;
            // Search for closing tag.
            while (pos < s.size()) {
                if (s[pos] == '$') {
                    size_t tag_end_start = pos;
                    pos++;
                    while (pos < s.size() &&
                           (std::isalnum(static_cast<unsigned char>(s[pos])) || s[pos] == '_')) {
                        pos++;
                    }
                    if (pos < s.size() && s[pos] == '$') {
                        std::string end_tag(s, tag_end_start, pos - tag_end_start + 1);
                        if (end_tag == tag) {
                            std::string content(s, content_start, tag_end_start - content_start);
                            pos++;  // skip closing $ of closing tag
                            return BisonParser::make_SCONST(content, tok_start);
                        }
                    }
                    pos = tag_end_start + 1;
                } else {
                    pos++;
                }
            }
            // Unterminated dollar string — fall through to return EOF.
            return BisonParser::make_YYEOF(tok_start);
        }
        // Not a dollar quote or parameter — just a $ sign (operator).
        pos = tag_start + 1;
        return BisonParser::make_Op(std::string("$"), tok_start);
    }

    // Multi-character operators (check two-character tokens first).
    if (pos + 1 < s.size()) {
        char c2 = s[pos + 1];
        // Typecast operator ::
        if (c == ':' && c2 == ':') {
            pos += 2;
            return BisonParser::make_TYPECAST(tok_start);
        }
        // Dot-dot ..
        if (c == '.' && c2 == '.') {
            pos += 2;
            return BisonParser::make_DOT_DOT(tok_start);
        }
        // Colon-equals :=
        if (c == ':' && c2 == '=') {
            pos += 2;
            return BisonParser::make_COLON_EQUALS(tok_start);
        }
        // Equals-greater =>
        if (c == '=' && c2 == '>') {
            pos += 2;
            return BisonParser::make_EQUALS_GREATER(tok_start);
        }
        // <= >= <>
        if (c == '<' && c2 == '=') {
            pos += 2;
            return BisonParser::make_LESS_EQUALS(tok_start);
        }
        if (c == '>' && c2 == '=') {
            pos += 2;
            return BisonParser::make_GREATER_EQUALS(tok_start);
        }
        if (c == '<' && c2 == '>') {
            pos += 2;
            return BisonParser::make_NOT_EQUALS(tok_start);
        }
    }

    // Single-character tokens — the token code is the ASCII value.
    pos++;
    switch (c) {
        case '(':
        case ')':
        case ',':
        case ';':
        case '.':
        case '+':
        case '-':
        case '*':
        case '/':
        case '%':
        case '^':
        case '<':
        case '>':
        case '=':
        case '[':
        case ']':
            return BisonParser::symbol_type(static_cast<int>(c), tok_start);
    }

    // Unknown character — return EOF to signal a scan error.
    return BisonParser::make_YYEOF(tok_start);
}

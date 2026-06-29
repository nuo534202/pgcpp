#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "parser/keywords.hpp"

namespace pgcpp::parser {

// Token location — byte offset from the start of the input string.
// PostgreSQL tracks only the start position, not end.
using YYLTYPE = int;

// Core semantic value returned by the scanner.
// This is a subset of the full YYSTYPE used by the grammar.
struct CoreYYSTYPE {
    int ival = 0;                           // integer literals
    std::string str;                        // identifiers and non-integer literals
    const KeywordEntry* keyword = nullptr;  // canonical keyword entry
};

// Scanner state. In PostgreSQL this is core_yy_extra_type.
// We keep the essential fields needed for SQL scanning.
struct ScannerState {
    std::string scanbuf;  // the input string being scanned
    int scanbuflen = 0;   // length of scanbuf

    // Scanner settings (initialized from GUCs in PostgreSQL)
    int backslash_quote = 1;  // BACKSLASH_QUOTE_SAFE_ENCODING
    bool escape_string_warning = true;
    bool standard_conforming_strings = true;

    // Literal accumulation buffer
    std::string literalbuf;
    int literallen = 0;

    // Comment nesting depth
    int xcdepth = 0;

    // Dollar quote start string
    std::string dolqstart;

    // Saved location for PUSH_YYLLOC
    YYLTYPE save_yylloc = 0;

    // UTF16 surrogate pair first part
    int32_t utf16_first_part = 0;

    // Warning state
    bool warn_on_first_escape = false;
    bool saw_non_ascii = false;

    // Current token location
    YYLTYPE yylloc = 0;
};

}  // namespace pgcpp::parser

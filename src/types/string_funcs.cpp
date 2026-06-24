// string_funcs.cpp — string function implementation.
//
// Converts PostgreSQL's utils/adt/like.c, utils/adt/regexp.c, and
// utils/adt/varchar.c (length function) to C++20. LIKE matching is
// implemented with a direct state machine (matching PostgreSQL's approach);
// regexp_replace/substring use C++ <regex>.

#include "mytoydb/types/string_funcs.h"

#include <cstring>
#include <regex>
#include <string>
#include <string_view>

#include "mytoydb/common/error/elog.h"
#include "mytoydb/common/memory/memory_context.h"
#include "mytoydb/types/builtins.h"

namespace mytoydb::types {

using mytoydb::error::LogLevel;
using mytoydb::memory::palloc;

namespace {

// Allocate a palloc'd C string copy (null-terminated).
char* PallocCString(std::string_view s) {
    char* buf = static_cast<char*>(palloc(s.size() + 1));
    if (!s.empty()) {
        std::memcpy(buf, s.data(), s.size());
    }
    buf[s.size()] = '\0';
    return buf;
}

// LIKE matching using a simple byte-by-byte state machine.
// This mirrors PostgreSQL's MatchText algorithm: % matches any sequence,
// _ matches any single character, and \ escapes the next character.
// Case-sensitive (PostgreSQL LIKE is case-sensitive by default).
bool MatchLike(std::string_view text, std::string_view pattern) {
    std::size_t t = 0;                            // text index
    std::size_t p = 0;                            // pattern index
    std::size_t t_star = std::string_view::npos;  // text position after last %
    std::size_t p_star = std::string_view::npos;  // pattern position after last %

    while (t < text.size()) {
        if (p < pattern.size()) {
            char pc = pattern[p];
            if (pc == '%') {
                // Skip consecutive % characters.
                do {
                    ++p;
                } while (p < pattern.size() && pattern[p] == '%');
                if (p >= pattern.size()) {
                    return true;  // trailing % matches everything
                }
                t_star = t;
                p_star = p;
                continue;
            }
            if (pc == '_') {
                ++p;
                ++t;
                continue;
            }
            if (pc == '\\' && p + 1 < pattern.size()) {
                ++p;  // skip the backslash, match the next char literally
                pc = pattern[p];
            }
            if (text[t] == pc) {
                ++p;
                ++t;
                continue;
            }
        }
        // Mismatch: backtrack to last % if any.
        if (p_star != std::string_view::npos) {
            p = p_star;
            ++t_star;
            t = t_star;
            continue;
        }
        return false;
    }

    // Text exhausted. Remaining pattern must be all % to match.
    while (p < pattern.size() && pattern[p] == '%') {
        ++p;
    }
    return p >= pattern.size();
}

// Convert a PostgreSQL-style replacement string (\1, \2, ...) to C++ regex
// replacement syntax ($1, $2, ...).
std::string ConvertReplacement(std::string_view pg_replacement) {
    std::string result;
    result.reserve(pg_replacement.size());
    for (std::size_t i = 0; i < pg_replacement.size(); ++i) {
        if (pg_replacement[i] == '\\' && i + 1 < pg_replacement.size()) {
            char next = pg_replacement[i + 1];
            if (next >= '0' && next <= '9') {
                result += '$';
                result += next;
                ++i;
            } else if (next == '\\') {
                result += '\\';
                ++i;
            } else {
                result += next;
                ++i;
            }
        } else {
            result += pg_replacement[i];
        }
    }
    return result;
}

}  // namespace

// --- length ---

Datum text_length(Datum value) {
    const char* text = DatumGetTextP(value);
    int data_len = VARSIZE_DATA(text);
    return Int32GetDatum(data_len);
}

// --- LIKE ---

Datum like(Datum text_datum, Datum pattern_datum) {
    std::string text = TextDatumToString(text_datum);
    std::string pattern = TextDatumToString(pattern_datum);
    return BoolGetDatum(MatchLike(text, pattern));
}

Datum not_like(Datum text_datum, Datum pattern_datum) {
    std::string text = TextDatumToString(text_datum);
    std::string pattern = TextDatumToString(pattern_datum);
    return BoolGetDatum(!MatchLike(text, pattern));
}

// --- regexp_replace ---

Datum regexp_replace(Datum source_datum, Datum pattern_datum, Datum replacement_datum) {
    std::string source = TextDatumToString(source_datum);
    std::string pattern = TextDatumToString(pattern_datum);
    std::string replacement = TextDatumToString(replacement_datum);

    std::string cpp_replacement = ConvertReplacement(replacement);

    try {
        std::regex re(pattern);
        std::string result =
            std::regex_replace(source, re, cpp_replacement, std::regex_constants::format_default);
        return MakeTextDatum(result);
    } catch (const std::regex_error& e) {
        ereport(LogLevel::kError,
                "regexp_replace: invalid regular expression: " + std::string(e.what()));
    }
}

// --- substring ---

Datum substring(Datum text_datum, Datum pattern_datum) {
    std::string text = TextDatumToString(text_datum);
    std::string pattern = TextDatumToString(pattern_datum);

    try {
        std::regex re(pattern);
        std::smatch match;
        if (std::regex_search(text, match, re)) {
            // If the pattern has capture groups, return the first capture group.
            // Otherwise, return the entire match.
            if (match.size() > 1) {
                return MakeTextDatum(match[1].str());
            }
            return MakeTextDatum(match[0].str());
        }
        return MakeTextDatum("");
    } catch (const std::regex_error& e) {
        ereport(LogLevel::kError,
                "substring: invalid regular expression: " + std::string(e.what()));
    }
}

}  // namespace mytoydb::types

// string_funcs.cpp — string function implementation.
//
// Converts PostgreSQL's utils/adt/like.c, utils/adt/regexp.c, and
// utils/adt/varchar.c (length function) to C++20. LIKE matching is
// implemented with a direct state machine (matching PostgreSQL's approach);
// regexp_replace/substring use C++ <regex>.

#include "pgcpp/types/string_funcs.hpp"

#include <cctype>
#include <cstring>
#include <regex>
#include <string>
#include <string_view>

#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/memory_context.hpp"
#include "pgcpp/types/builtins.hpp"

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

// Returns true if `flags` contains the character `flag`.
bool FlagContains(std::string_view flags, char flag) {
    return flags.find(flag) != std::string_view::npos;
}

// True if the byte is ASCII whitespace (space, tab, newline, CR, FF, VT).
bool IsAsciiSpace(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
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

// 3-arg form: PostgreSQL's default — replace only the FIRST match.
Datum regexp_replace(Datum source_datum, Datum pattern_datum, Datum replacement_datum) {
    std::string source = TextDatumToString(source_datum);
    std::string pattern = TextDatumToString(pattern_datum);
    std::string replacement = TextDatumToString(replacement_datum);
    std::string cpp_replacement = ConvertReplacement(replacement);

    try {
        std::regex re(pattern);
        // format_first_only — replace only the first match (PG default, no 'g').
        std::string result = std::regex_replace(source, re, cpp_replacement,
                                                std::regex_constants::format_first_only);
        return MakeTextDatum(result);
    } catch (const std::regex_error& e) {
        ereport(LogLevel::kError,
                "regexp_replace: invalid regular expression: " + std::string(e.what()));
    }
}

// 4-arg form: flags containing 'g' replaces ALL matches; otherwise first only.
Datum regexp_replace(Datum source_datum, Datum pattern_datum, Datum replacement_datum,
                     Datum flags_datum) {
    std::string source = TextDatumToString(source_datum);
    std::string pattern = TextDatumToString(pattern_datum);
    std::string replacement = TextDatumToString(replacement_datum);
    std::string flags = TextDatumToString(flags_datum);
    std::string cpp_replacement = ConvertReplacement(replacement);

    try {
        std::regex re(pattern);
        std::regex_constants::match_flag_type how = FlagContains(flags, 'g')
                                                        ? std::regex_constants::format_default
                                                        : std::regex_constants::format_first_only;
        std::string result = std::regex_replace(source, re, cpp_replacement, how);
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

// --- textcat (alias of text_concat) ---

Datum textcat(Datum a, Datum b) {
    return text_concat(a, b);
}

// --- text_substr ---

Datum text_substr(Datum str_datum, Datum start_datum, Datum len_datum) {
    std::string s = TextDatumToString(str_datum);
    int32_t start = DatumGetInt32(start_datum);
    int32_t len = DatumGetInt32(len_datum);

    // PostgreSQL's `from` is 1-based. Negative `from` is treated as 1.
    // Negative `len` is treated as 0 (returns empty string).
    if (start < 1) {
        start = 1;
    }
    if (len < 0) {
        len = 0;
    }
    std::size_t begin = static_cast<std::size_t>(start - 1);
    if (begin >= s.size()) {
        return MakeTextDatum("");
    }
    std::size_t take = static_cast<std::size_t>(len);
    if (take > s.size() - begin) {
        take = s.size() - begin;
    }
    return MakeTextDatum(s.substr(begin, take));
}

// --- text_left / text_right ---

Datum text_left(Datum str_datum, Datum n_datum) {
    std::string s = TextDatumToString(str_datum);
    int32_t n = DatumGetInt32(n_datum);
    if (n < 0) {
        // Negative n: return all but the last |n| characters.
        std::size_t drop = static_cast<std::size_t>(-n);
        if (drop >= s.size()) {
            return MakeTextDatum("");
        }
        return MakeTextDatum(s.substr(0, s.size() - drop));
    }
    std::size_t take = static_cast<std::size_t>(n);
    if (take > s.size()) {
        take = s.size();
    }
    return MakeTextDatum(s.substr(0, take));
}

Datum text_right(Datum str_datum, Datum n_datum) {
    std::string s = TextDatumToString(str_datum);
    int32_t n = DatumGetInt32(n_datum);
    if (n < 0) {
        // Negative n: return all but the first |n| characters.
        std::size_t drop = static_cast<std::size_t>(-n);
        if (drop >= s.size()) {
            return MakeTextDatum("");
        }
        return MakeTextDatum(s.substr(drop));
    }
    std::size_t take = static_cast<std::size_t>(n);
    if (take >= s.size()) {
        return MakeTextDatum(s);
    }
    return MakeTextDatum(s.substr(s.size() - take));
}

// --- text_upper / text_lower ---

Datum text_upper(Datum str_datum) {
    std::string s = TextDatumToString(str_datum);
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return MakeTextDatum(s);
}

Datum text_lower(Datum str_datum) {
    std::string s = TextDatumToString(str_datum);
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return MakeTextDatum(s);
}

// --- text_btrim / text_ltrim / text_rtrim ---

Datum text_btrim(Datum str_datum) {
    std::string s = TextDatumToString(str_datum);
    std::size_t begin = 0;
    std::size_t end = s.size();
    while (begin < end && IsAsciiSpace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    while (end > begin && IsAsciiSpace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return MakeTextDatum(s.substr(begin, end - begin));
}

Datum text_ltrim(Datum str_datum) {
    std::string s = TextDatumToString(str_datum);
    std::size_t begin = 0;
    while (begin < s.size() && IsAsciiSpace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    return MakeTextDatum(s.substr(begin));
}

Datum text_rtrim(Datum str_datum) {
    std::string s = TextDatumToString(str_datum);
    std::size_t end = s.size();
    while (end > 0 && IsAsciiSpace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return MakeTextDatum(s.substr(0, end));
}

// --- text_repeat ---

Datum text_repeat(Datum str_datum, Datum n_datum) {
    std::string s = TextDatumToString(str_datum);
    int32_t n = DatumGetInt32(n_datum);
    if (n <= 0 || s.empty()) {
        return MakeTextDatum("");
    }
    std::string result;
    result.reserve(static_cast<std::size_t>(n) * s.size());
    for (int i = 0; i < n; ++i) {
        result += s;
    }
    return MakeTextDatum(result);
}

// --- text_reverse ---

Datum text_reverse(Datum str_datum) {
    std::string s = TextDatumToString(str_datum);
    std::string reversed(s.rbegin(), s.rend());
    return MakeTextDatum(reversed);
}

}  // namespace mytoydb::types

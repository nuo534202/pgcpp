// regprefix.cpp — pg_regprefix implementation.
//
// Extracts the longest literal prefix of a compiled pattern that is anchored at
// the start. This is a pragmatic static analysis of the pattern text (re->pattern)
// rather than a full NFA traversal as PostgreSQL does: it scans the pattern left
// to right, requires a leading '^', then collects literal characters until the
// first metacharacter. This covers the common index-optimization cases used by
// pgcpp's planner (e.g. '^prefix.*', '^literal$').

#include "regex/regprefix.hpp"

#include <cstring>
#include <string>
#include <string_view>

#include "common/memory/memory_context.hpp"

namespace pgcpp::regex {

using pgcpp::memory::palloc;

int pg_regprefix(const regex_t* re, char** prefix, std::size_t* prefix_size) {
    if (re == nullptr || prefix == nullptr || prefix_size == nullptr) {
        return -1;
    }
    *prefix = nullptr;
    *prefix_size = 0;
    if (re->re_magic != kRegexMagic || re->re_errno != kRegOk) {
        return -1;
    }

    std::string_view pat(re->pattern);
    if (pat.empty() || pat[0] != '^') {
        // Not anchored at the start — no fixed positional prefix.
        return -1;
    }

    // Collect literal characters after '^'. Stop at the first regex
    // metacharacter. '\' is treated conservatively as a terminator because
    // distinguishing '\.' (literal dot) from '\d' (character class) would require
    // deeper syntax analysis than this best-effort extractor performs.
    std::size_t i = 1;
    std::size_t prefix_len = 0;
    while (i < pat.size()) {
        char c = pat[i];
        if (c == '.' || c == '*' || c == '+' || c == '?' || c == '[' || c == '(' || c == '|' ||
            c == '{' || c == '\\' || c == '$' || c == '^') {
            break;
        }
        ++prefix_len;
        ++i;
    }

    if (prefix_len == 0) {
        return -1;
    }

    char* buf = static_cast<char*>(palloc(prefix_len + 1));
    std::memcpy(buf, pat.data() + 1, prefix_len);
    buf[prefix_len] = '\0';
    *prefix = buf;
    *prefix_size = prefix_len;
    return static_cast<int>(prefix_len);
}

}  // namespace pgcpp::regex

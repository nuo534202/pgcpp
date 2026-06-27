// numutils.cpp — numeric utility functions.
//
// Provides PostgreSQL-compatible helpers used across the types module:
//   * pg_strtoint64: strict int64 parsing (ereport on bad input).
//   * float8_out_internal: shortest-round-trip double formatting via
//     std::to_chars, with PG-style special values ("Infinity", "NaN") and
//     PG-style scientific notation ("1e+20" instead of "1e20").

#include "mytoydb/types/numutils.hpp"

#include <cctype>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>

#include "mytoydb/common/error/elog.hpp"
#include "mytoydb/common/memory/memory_context.hpp"

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

}  // namespace

int64_t pg_strtoint64(const char* str) {
    if (str == nullptr) {
        ereport(LogLevel::kError, "invalid input syntax for type bigint: NULL");
    }
    errno = 0;
    char* endptr = nullptr;
    long long val = std::strtoll(str, &endptr, 10);
    if (errno == ERANGE || val < INT64_MIN || val > INT64_MAX) {
        ereport(LogLevel::kError,
                "value \"" + std::string(str) + "\" is out of range for type bigint");
    }
    if (endptr == str) {
        ereport(LogLevel::kError,
                "invalid input syntax for type bigint: \"" + std::string(str) + "\"");
    }
    while (*endptr != '\0' && std::isspace(static_cast<unsigned char>(*endptr))) {
        ++endptr;
    }
    if (*endptr != '\0') {
        ereport(LogLevel::kError,
                "invalid input syntax for type bigint: \"" + std::string(str) + "\"");
    }
    return static_cast<int64_t>(val);
}

char* float8_out_internal(double val) {
    // PostgreSQL conventions for non-finite values.
    if (std::isnan(val)) {
        return PallocCString("NaN");
    }
    if (std::isinf(val)) {
        return PallocCString(val > 0 ? "Infinity" : "-Infinity");
    }
    // PostgreSQL normalizes -0.0 to "0" (no negative sign).
    if (val == 0.0) {
        return PallocCString("0");
    }

    // std::to_chars produces the shortest decimal representation that
    // round-trips through strtod (matches PG's extra_float_digits=1 default).
    char buf[64];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), val);
    if (ec != std::errc{}) {
        // Should not happen for finite doubles with a 64-byte buffer.
        std::snprintf(buf, sizeof(buf), "%.17g", val);
        return PallocCString(buf);
    }
    *ptr = '\0';
    std::string s(buf);

    // PostgreSQL prints a '+' after 'e' for positive exponents ("1e+20"),
    // while std::to_chars omits it ("1e20"). Normalize to PG's form.
    std::size_t e_pos = s.find('e');
    if (e_pos != std::string::npos && e_pos + 1 < s.size() && s[e_pos + 1] != '+' &&
        s[e_pos + 1] != '-') {
        s.insert(e_pos + 1, 1, '+');
    }
    return PallocCString(s);
}

}  // namespace mytoydb::types

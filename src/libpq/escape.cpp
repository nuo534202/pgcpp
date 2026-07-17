// escape.cpp — String / identifier / bytea escape utilities (P3-11).
//
// Mirrors libpq's PQescapeString / PQescapeLiteral / PQescapeIdentifier /
// PQescapeBytea / PQunescapeBytea. See libpq/escape.hpp for the API.
//
// All variants assume standard_conforming_strings=on (the pgcpp default),
// so backslash is a literal character inside SQL string literals. The
// connection-scoped variants ignore the PgConn argument because the
// escaping depends only on the protocol version (3) and standard_conforming_strings.
#include "libpq/escape.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace pgcpp::libpq {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

// Hex digit -> integer value, or -1 if not a hex digit.
int HexVal(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

char HexChar(int v) {
    return static_cast<char>(v < 10 ? '0' + v : 'a' + (v - 10));
}

// Validate that `str` is a valid SQL identifier (mirrors libpq's
// PQescapeIdentifier checks): non-empty, length <= NAMEDATALEN-1 (=63),
// starts with letter or underscore, contains only [A-Za-z0-9_$].
bool IsValidIdentifier(const std::string& str) {
    if (str.empty() || str.size() > 63)
        return false;
    char first = str[0];
    if (!std::isalpha(static_cast<unsigned char>(first)) && first != '_')
        return false;
    for (char c : str) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '_' && c != '$')
            return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

EscapeStringResult EscapeString(const std::string& str) {
    EscapeStringResult r;
    r.ok = true;
    // With standard_conforming_strings=on, only the single quote needs
    // to be doubled. We do not double backslashes (libpq behaviour under
    // standard_conforming_strings).
    r.out.push_back('\'');
    for (char c : str) {
        if (c == '\'') {
            r.out.push_back('\'');  // double the quote
        }
        r.out.push_back(c);
    }
    r.out.push_back('\'');
    return r;
}

EscapeStringResult EscapeStringConn(const PgConn& /*conn*/, const std::string& str) {
    return EscapeString(str);
}

EscapeStringResult EscapeLiteral(const std::string& str) {
    // PQescapeLiteral wraps EscapeString's output with surrounding quotes
    // (already done by EscapeString).
    return EscapeString(str);
}

EscapeStringResult EscapeLiteralConn(const PgConn& conn, const std::string& str) {
    return EscapeStringConn(conn, str);
}

EscapeStringResult EscapeIdentifier(const std::string& str) {
    EscapeStringResult r;
    if (!IsValidIdentifier(str)) {
        r.ok = false;
        if (str.empty()) {
            r.error_message = "identifier is empty";
        } else if (str.size() > 63) {
            r.error_message = "identifier too long (max 63 bytes)";
        } else {
            r.error_message = "identifier contains invalid characters";
        }
        return r;
    }
    r.ok = true;
    r.out.push_back('"');
    for (char c : str) {
        if (c == '"')
            r.out.push_back('"');  // double the quote
        r.out.push_back(c);
    }
    r.out.push_back('"');
    return r;
}

EscapeStringResult EscapeIdentifierConn(const PgConn& /*conn*/, const std::string& str) {
    return EscapeIdentifier(str);
}

std::string QuoteLiteral(const std::string& str) {
    std::string out;
    out.push_back('\'');
    for (char c : str) {
        if (c == '\'')
            out.push_back('\'');
        out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

std::string QuoteIdentifier(const std::string& str) {
    std::string out;
    out.push_back('"');
    for (char c : str) {
        if (c == '"')
            out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::string EscapeBytea(const unsigned char* data, std::size_t len) {
    // Modern hex format: "\\x" + hex digits.
    std::string out;
    out.reserve(2 + len * 2);
    out.push_back('\\');
    out.push_back('x');
    for (std::size_t i = 0; i < len; ++i) {
        unsigned char b = data[i];
        out.push_back(HexChar((b >> 4) & 0x0F));
        out.push_back(HexChar(b & 0x0F));
    }
    return out;
}

std::string EscapeByteaConn(const PgConn& /*conn*/, const unsigned char* data, std::size_t len) {
    return EscapeBytea(data, len);
}

UnescapeByteaResult UnescapeBytea(const char* str, std::size_t len) {
    UnescapeByteaResult r;
    if (str == nullptr) {
        r.ok = false;
        r.error_message = "null input";
        return r;
    }
    // Detect hex format: starts with "\\x".
    if (len >= 2 && str[0] == '\\' && str[1] == 'x') {
        const char* p = str + 2;
        const char* end = str + len;
        while (p + 1 < end) {
            int hi = HexVal(p[0]);
            int lo = HexVal(p[1]);
            if (hi < 0 || lo < 0) {
                r.ok = false;
                r.error_message = "invalid hex digit in bytea literal";
                return r;
            }
            r.out.push_back(static_cast<unsigned char>((hi << 4) | lo));
            p += 2;
        }
        r.ok = true;
        return r;
    }
    // Octal escape form: "\nnn" or "\\" or raw byte.
    std::size_t i = 0;
    while (i < len) {
        if (str[i] == '\\' && i + 1 < len) {
            char next = str[i + 1];
            if (next == '\\') {
                r.out.push_back('\\');
                i += 2;
                continue;
            }
            // Octal escape: up to 3 octal digits.
            if (next >= '0' && next <= '7') {
                int val = 0;
                int count = 0;
                while (count < 3 && i + 1 + count < len && str[i + 1 + count] >= '0' &&
                       str[i + 1 + count] <= '7') {
                    val = val * 8 + (str[i + 1 + count] - '0');
                    ++count;
                }
                r.out.push_back(static_cast<unsigned char>(val));
                i += 1 + count;
                continue;
            }
            // Unknown escape — keep as-is.
            r.out.push_back('\\');
            ++i;
        } else {
            r.out.push_back(static_cast<unsigned char>(str[i]));
            ++i;
        }
    }
    r.ok = true;
    return r;
}

}  // namespace pgcpp::libpq

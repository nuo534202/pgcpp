// wchar.hpp — multibyte character width and wide-char conversion.
//
// Converted from PostgreSQL 15's src/backend/utils/mb/wchar.c.
//
// pgcpp uses UTF-8 as the primary encoding. This header provides functions
// to determine the byte length of a multibyte character, convert between
// multibyte (UTF-8) and wide-character (uint32_t Unicode codepoint)
// representations, and count characters in a multibyte string.
//
// For SQL_ASCII and single-byte encodings (LATIN1 etc.), every byte is one
// character. For UTF-8, characters are 1-4 bytes. For GBK/BIG5, characters
// are 1-2 bytes.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "utils/mb/encoding.hpp"

namespace pgcpp::utils {

// PgMblen — return the byte length of the multibyte character starting at
// the given byte. For a leading byte of a multibyte sequence, returns the
// total length (1-4). For a continuation byte, returns 1.
// Returns 0 for null or empty input.
int PgMblen(PgEncoding enc, const char* mbstr);

// PgMblenUtf8 — UTF-8 specific byte length from the first byte.
int PgMblenUtf8(unsigned char c);

// PgDsplen — return the display width of the character (1 for ASCII,
// 1 for most CJK, 0 for combining marks in a simplified model).
int PgDsplen(PgEncoding enc, const char* mbstr);

// PgMbstrlen — count the number of characters (not bytes) in a
// null-terminated multibyte string.
int PgMbstrlen(PgEncoding enc, const char* mbstr);

// PgMbstrlenWithLen — count characters in a length-limited multibyte string.
int PgMbstrlenWithLen(PgEncoding enc, const char* mbstr, int limit);

// PgMb2wchar — convert a UTF-8 (or other encoding) string to a vector of
// wide characters (Unicode codepoints). Returns the number of characters.
// For non-UTF-8 encodings, each byte is treated as a single codepoint
// (sufficient for ASCII and LATIN1).
int PgMb2wchar(PgEncoding enc, std::string_view mbstr, std::vector<uint32_t>* wchars);

// PgWchar2Mb — convert wide characters (Unicode codepoints) to a UTF-8
// (or other encoding) byte string. For non-UTF-8 encodings, only codepoints
// < 256 are representable (LATIN1 range); others are replaced with '?'.
std::string PgWchar2Mb(PgEncoding enc, const std::vector<uint32_t>& wchars);

// Utf8ToCodepoint — decode a single UTF-8 character starting at `pos`.
// Returns the codepoint and advances `pos` past the character.
// On invalid UTF-8, returns 0xFFFD (replacement char) and advances by 1.
uint32_t Utf8ToCodepoint(std::string_view str, size_t* pos);

// CodepointToUtf8 — encode a Unicode codepoint as UTF-8 bytes.
std::string CodepointToUtf8(uint32_t cp);

}  // namespace pgcpp::utils

// wchar_test.cpp — Unit tests for multibyte character width (P2-6).
//
// Tests UTF-8 byte length (PgMblen), character counting (PgMbstrlen),
// UTF-8 decode/encode (Utf8ToCodepoint/CodepointToUtf8), and
// wide-char conversion (PgMb2wchar/PgWchar2Mb).

#include "utils/mb/wchar.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using pgcpp::utils::CodepointToUtf8;
using pgcpp::utils::PgEncoding;
using pgcpp::utils::PgMb2wchar;
using pgcpp::utils::PgMblen;
using pgcpp::utils::PgMblenUtf8;
using pgcpp::utils::PgMbstrlen;
using pgcpp::utils::PgMbstrlenWithLen;
using pgcpp::utils::PgWchar2Mb;
using pgcpp::utils::Utf8ToCodepoint;

// ===========================================================================
// PgMblenUtf8 — byte length from first byte
// ===========================================================================

TEST(WcharTest, PgMblenUtf8Ascii) {
    EXPECT_EQ(PgMblenUtf8('A'), 1);
    EXPECT_EQ(PgMblenUtf8('\0'), 1);
    EXPECT_EQ(PgMblenUtf8(0x7F), 1);
}

TEST(WcharTest, PgMblenUtf8TwoByte) {
    EXPECT_EQ(PgMblenUtf8(0xC2), 2);  // U+0080
    EXPECT_EQ(PgMblenUtf8(0xDF), 2);  // U+07FF
}

TEST(WcharTest, PgMblenUtf8ThreeByte) {
    EXPECT_EQ(PgMblenUtf8(0xE0), 3);  // U+0800
    EXPECT_EQ(PgMblenUtf8(0xEF), 3);  // U+FFFF
}

TEST(WcharTest, PgMblenUtf8FourByte) {
    EXPECT_EQ(PgMblenUtf8(0xF0), 4);  // U+10000
    EXPECT_EQ(PgMblenUtf8(0xF4), 4);  // U+10FFFF
}

TEST(WcharTest, PgMblenUtf8ContinuationByte) {
    // Continuation bytes (0x80-0xBF) are treated as 1-byte characters.
    EXPECT_EQ(PgMblenUtf8(0x80), 1);
    EXPECT_EQ(PgMblenUtf8(0xBF), 1);
}

// ===========================================================================
// PgMblen — encoding-aware byte length
// ===========================================================================

TEST(WcharTest, PgMblenUtf8Encoding) {
    EXPECT_EQ(PgMblen(PgEncoding::kUtf8, "A"), 1);
    // U+00E9 (é) = 0xC3 0xA9 in UTF-8
    EXPECT_EQ(PgMblen(PgEncoding::kUtf8, "\xC3\xA9"), 2);
    // U+4E2D (中) = 0xE4 0xB8 0xAD in UTF-8
    EXPECT_EQ(PgMblen(PgEncoding::kUtf8, "\xE4\xB8\xAD"), 3);
    // U+1F600 (😀) = 0xF0 0x9F 0x98 0x80 in UTF-8
    EXPECT_EQ(PgMblen(PgEncoding::kUtf8, "\xF0\x9F\x98\x80"), 4);
}

TEST(WcharTest, PgMblenSingleByteEncoding) {
    EXPECT_EQ(PgMblen(PgEncoding::kSqlAscii, "A"), 1);
    EXPECT_EQ(PgMblen(PgEncoding::kLatin1, "\xFF"), 1);
}

TEST(WcharTest, PgMblenGbk) {
    // ASCII byte: 1 byte
    EXPECT_EQ(PgMblen(PgEncoding::kGbk, "A"), 1);
    // Lead byte >= 0x80: 2 bytes
    EXPECT_EQ(PgMblen(PgEncoding::kGbk, "\x81\x40"), 2);
}

TEST(WcharTest, PgMblenNull) {
    EXPECT_EQ(PgMblen(PgEncoding::kUtf8, nullptr), 0);
    EXPECT_EQ(PgMblen(PgEncoding::kUtf8, ""), 0);
}

// ===========================================================================
// PgMbstrlen — character counting
// ===========================================================================

TEST(WcharTest, PgMbstrlenAscii) {
    EXPECT_EQ(PgMbstrlen(PgEncoding::kUtf8, "hello"), 5);
    EXPECT_EQ(PgMbstrlen(PgEncoding::kUtf8, ""), 0);
    EXPECT_EQ(PgMbstrlen(PgEncoding::kUtf8, "a"), 1);
}

TEST(WcharTest, PgMbstrlenUtf8Multibyte) {
    // "héllo" = h + é(2 bytes) + l + l + o = 5 chars, 6 bytes
    EXPECT_EQ(PgMbstrlen(PgEncoding::kUtf8, "héllo"), 5);
    // "中文" = 2 chars, 6 bytes
    EXPECT_EQ(PgMbstrlen(PgEncoding::kUtf8, "中文"), 2);
    // "😀" = 1 char, 4 bytes
    EXPECT_EQ(PgMbstrlen(PgEncoding::kUtf8, "😀"), 1);
}

TEST(WcharTest, PgMbstrlenWithLen) {
    // Count characters in a length-limited string.
    std::string s = "héllo";  // 6 bytes, 5 chars
    EXPECT_EQ(PgMbstrlenWithLen(PgEncoding::kUtf8, s.data(), 6), 5);
    EXPECT_EQ(PgMbstrlenWithLen(PgEncoding::kUtf8, s.data(), 3), 2);  // "hé"
}

TEST(WcharTest, PgMbstrlenSingleByte) {
    EXPECT_EQ(PgMbstrlen(PgEncoding::kLatin1, "hello"), 5);
    EXPECT_EQ(PgMbstrlen(PgEncoding::kSqlAscii, "hello"), 5);
}

// ===========================================================================
// Utf8ToCodepoint — UTF-8 decoding
// ===========================================================================

TEST(WcharTest, Utf8ToCodepointAscii) {
    std::string s = "A";
    size_t pos = 0;
    EXPECT_EQ(Utf8ToCodepoint(s, &pos), 'A');
    EXPECT_EQ(pos, 1u);
}

TEST(WcharTest, Utf8ToCodepointTwoByte) {
    std::string s = "\xC3\xA9";  // é = U+00E9
    size_t pos = 0;
    EXPECT_EQ(Utf8ToCodepoint(s, &pos), 0x00E9u);
    EXPECT_EQ(pos, 2u);
}

TEST(WcharTest, Utf8ToCodepointThreeByte) {
    std::string s = "\xE4\xB8\xAD";  // 中 = U+4E2D
    size_t pos = 0;
    EXPECT_EQ(Utf8ToCodepoint(s, &pos), 0x4E2Du);
    EXPECT_EQ(pos, 3u);
}

TEST(WcharTest, Utf8ToCodepointFourByte) {
    std::string s = "\xF0\x9F\x98\x80";  // 😀 = U+1F600
    size_t pos = 0;
    EXPECT_EQ(Utf8ToCodepoint(s, &pos), 0x1F600u);
    EXPECT_EQ(pos, 4u);
}

TEST(WcharTest, Utf8ToCodepointMultipleChars) {
    std::string s = "AB中";  // A, B, 中
    size_t pos = 0;
    EXPECT_EQ(Utf8ToCodepoint(s, &pos), 'A');
    EXPECT_EQ(Utf8ToCodepoint(s, &pos), 'B');
    EXPECT_EQ(Utf8ToCodepoint(s, &pos), 0x4E2Du);
    EXPECT_EQ(pos, s.size());
}

// ===========================================================================
// CodepointToUtf8 — UTF-8 encoding
// ===========================================================================

TEST(WcharTest, CodepointToUtf8Ascii) {
    EXPECT_EQ(CodepointToUtf8('A'), "A");
    EXPECT_EQ(CodepointToUtf8(0), std::string("\0", 1));
}

TEST(WcharTest, CodepointToUtf8TwoByte) {
    EXPECT_EQ(CodepointToUtf8(0x00E9), "\xC3\xA9");  // é
}

TEST(WcharTest, CodepointToUtf8ThreeByte) {
    EXPECT_EQ(CodepointToUtf8(0x4E2D), "\xE4\xB8\xAD");  // 中
}

TEST(WcharTest, CodepointToUtf8FourByte) {
    EXPECT_EQ(CodepointToUtf8(0x1F600), "\xF0\x9F\x98\x80");  // 😀
}

TEST(WcharTest, CodepointToUtf8Invalid) {
    // Invalid codepoint > U+10FFFF returns replacement char.
    EXPECT_EQ(CodepointToUtf8(0x110000), "\xEF\xBF\xBD");
}

// ===========================================================================
// Round-trip: codepoint -> UTF-8 -> codepoint
// ===========================================================================

TEST(WcharTest, RoundTripCodepoint) {
    const uint32_t codepoints[] = {'A', 'Z', 0x00E9, 0x4E2D, 0x1F600, 0x00FF};
    for (uint32_t cp : codepoints) {
        std::string utf8 = CodepointToUtf8(cp);
        size_t pos = 0;
        uint32_t decoded = Utf8ToCodepoint(utf8, &pos);
        EXPECT_EQ(decoded, cp) << "Round-trip failed for U+" << cp;
        EXPECT_EQ(pos, utf8.size());
    }
}

// ===========================================================================
// PgMb2wchar / PgWchar2Mb — wide char conversion
// ===========================================================================

TEST(WcharTest, PgMb2wcharUtf8) {
    std::string s = "A中";  // A + 中(U+4E2D)
    std::vector<uint32_t> wchars;
    int count = PgMb2wchar(PgEncoding::kUtf8, s, &wchars);
    EXPECT_EQ(count, 2);
    ASSERT_EQ(wchars.size(), 2u);
    EXPECT_EQ(wchars[0], 'A');
    EXPECT_EQ(wchars[1], 0x4E2Du);
}

TEST(WcharTest, PgMb2wcharSingleByte) {
    std::string s = "AB";
    std::vector<uint32_t> wchars;
    int count = PgMb2wchar(PgEncoding::kLatin1, s, &wchars);
    EXPECT_EQ(count, 2);
    EXPECT_EQ(wchars[0], 'A');
    EXPECT_EQ(wchars[1], 'B');
}

TEST(WcharTest, PgWchar2MbUtf8) {
    std::vector<uint32_t> wchars = {'A', 0x4E2D};  // A + 中
    std::string result = PgWchar2Mb(PgEncoding::kUtf8, wchars);
    EXPECT_EQ(result, "A中");
}

TEST(WcharTest, PgWchar2MbSingleByte) {
    std::vector<uint32_t> wchars = {'A', 'B'};
    std::string result = PgWchar2Mb(PgEncoding::kLatin1, wchars);
    EXPECT_EQ(result, "AB");
}

TEST(WcharTest, PgWchar2MbSingleByteOutOfRange) {
    // Codepoint > 255 is replaced with '?' for single-byte encodings.
    std::vector<uint32_t> wchars = {'A', 0x4E2D};
    std::string result = PgWchar2Mb(PgEncoding::kLatin1, wchars);
    EXPECT_EQ(result, "A?");
}

TEST(WcharTest, RoundTripMb2wchar2Mb) {
    std::string original = "Hello世界😀";  // ASCII + CJK + emoji
    std::vector<uint32_t> wchars;
    PgMb2wchar(PgEncoding::kUtf8, original, &wchars);
    std::string result = PgWchar2Mb(PgEncoding::kUtf8, wchars);
    EXPECT_EQ(result, original);
}

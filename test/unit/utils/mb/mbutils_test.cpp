// mbutils_test.cpp — Unit tests for multibyte encoding utilities (P2-6).
//
// Tests database/client encoding management, encoding conversion
// (UTF-8 <-> LATIN1, UTF-8 <-> GBK), and string validation.

#include "utils/mb/mbutils.hpp"

#include <gtest/gtest.h>

#include <string>

using pgcpp::utils::GetClientEncoding;
using pgcpp::utils::GetDatabaseEncoding;
using pgcpp::utils::GetDatabaseEncodingName;
using pgcpp::utils::PgConvert;
using pgcpp::utils::PgConvertFrom;
using pgcpp::utils::PgConvertTo;
using pgcpp::utils::PgEncoding;
using pgcpp::utils::PgValidServerEncoding;
using pgcpp::utils::PgVerifyMbstr;
using pgcpp::utils::SetClientEncoding;
using pgcpp::utils::SetDatabaseEncoding;

// ===========================================================================
// Database encoding management
// ===========================================================================

TEST(MbutilsTest, DefaultDatabaseEncodingIsUtf8) {
    EXPECT_EQ(GetDatabaseEncoding(), PgEncoding::kUtf8);
}

TEST(MbutilsTest, SetDatabaseEncoding) {
    SetDatabaseEncoding(PgEncoding::kLatin1);
    EXPECT_EQ(GetDatabaseEncoding(), PgEncoding::kLatin1);
    // Setting database encoding also sets client encoding.
    EXPECT_EQ(GetClientEncoding(), PgEncoding::kLatin1);
    // Restore default.
    SetDatabaseEncoding(PgEncoding::kUtf8);
}

TEST(MbutilsTest, GetDatabaseEncodingName) {
    SetDatabaseEncoding(PgEncoding::kUtf8);
    EXPECT_STREQ(GetDatabaseEncodingName(), "UTF8");
    SetDatabaseEncoding(PgEncoding::kLatin1);
    EXPECT_STREQ(GetDatabaseEncodingName(), "LATIN1");
    // Restore default.
    SetDatabaseEncoding(PgEncoding::kUtf8);
}

// ===========================================================================
// Client encoding management
// ===========================================================================

TEST(MbutilsTest, SetClientEncoding) {
    SetDatabaseEncoding(PgEncoding::kUtf8);
    SetClientEncoding(PgEncoding::kLatin1);
    EXPECT_EQ(GetClientEncoding(), PgEncoding::kLatin1);
    EXPECT_EQ(GetDatabaseEncoding(), PgEncoding::kUtf8);
    // Restore.
    SetClientEncoding(PgEncoding::kUtf8);
}

// ===========================================================================
// PgConvert — same encoding (no conversion)
// ===========================================================================

TEST(MbutilsTest, ConvertSameEncoding) {
    std::string s = "hello";
    EXPECT_EQ(PgConvert(PgEncoding::kUtf8, PgEncoding::kUtf8, s), s);
}

TEST(MbutilsTest, ConvertSqlAsciiPassthrough) {
    std::string s = "hello";
    // SQL_ASCII is passthrough — no conversion.
    EXPECT_EQ(PgConvert(PgEncoding::kSqlAscii, PgEncoding::kUtf8, s), s);
    EXPECT_EQ(PgConvert(PgEncoding::kUtf8, PgEncoding::kSqlAscii, s), s);
}

// ===========================================================================
// PgConvert — UTF-8 <-> LATIN1
// ===========================================================================

TEST(MbutilsTest, ConvertLatin1ToUtf8Ascii) {
    std::string s = "hello";
    std::string result = PgConvert(PgEncoding::kLatin1, PgEncoding::kUtf8, s);
    EXPECT_EQ(result, "hello");
}

TEST(MbutilsTest, ConvertLatin1ToUtf8HighByte) {
    // LATIN1 byte 0xE9 = é (U+00E9), UTF-8 = 0xC3 0xA9
    std::string latin1 = "\xE9";
    std::string utf8 = PgConvert(PgEncoding::kLatin1, PgEncoding::kUtf8, latin1);
    EXPECT_EQ(utf8, "\xC3\xA9");
}

TEST(MbutilsTest, ConvertUtf8ToLatin1HighByte) {
    // UTF-8 0xC3 0xA9 = é (U+00E9), LATIN1 = 0xE9
    std::string utf8 = "\xC3\xA9";
    std::string latin1 = PgConvert(PgEncoding::kUtf8, PgEncoding::kLatin1, utf8);
    EXPECT_EQ(latin1, "\xE9");
}

TEST(MbutilsTest, ConvertUtf8ToLatin1OutOfRange) {
    // UTF-8 中 (U+4E2D) is not representable in LATIN1 -> '?'
    std::string utf8 = "\xE4\xB8\xAD";  // 中
    std::string latin1 = PgConvert(PgEncoding::kUtf8, PgEncoding::kLatin1, utf8);
    EXPECT_EQ(latin1, "?");
}

TEST(MbutilsTest, RoundTripLatin1Utf8) {
    // Test all LATIN1 bytes 0x00-0xFF round-trip correctly.
    for (int i = 0; i < 256; i++) {
        std::string latin1(1, static_cast<char>(i));
        std::string utf8 = PgConvert(PgEncoding::kLatin1, PgEncoding::kUtf8, latin1);
        std::string back = PgConvert(PgEncoding::kUtf8, PgEncoding::kLatin1, utf8);
        EXPECT_EQ(back, latin1) << "Round-trip failed for byte 0x" << std::hex << i;
    }
}

// ===========================================================================
// PgConvertTo / PgConvertFrom
// ===========================================================================

TEST(MbutilsTest, ConvertToUsesDatabaseEncoding) {
    SetDatabaseEncoding(PgEncoding::kUtf8);
    std::string s = "\xE9";  // LATIN1 é
    // Convert from LATIN1 to database encoding (UTF-8).
    std::string result = PgConvertFrom(PgEncoding::kLatin1, s);
    EXPECT_EQ(result, "\xC3\xA9");

    // Convert from database encoding (UTF-8) to LATIN1.
    std::string back = PgConvertTo(PgEncoding::kLatin1, result);
    EXPECT_EQ(back, "\xE9");
}

// ===========================================================================
// PgVerifyMbstr — string validation
// ===========================================================================

TEST(MbutilsTest, VerifyMbstrSqlAscii) {
    // Any byte sequence is valid SQL_ASCII.
    EXPECT_TRUE(PgVerifyMbstr(PgEncoding::kSqlAscii, "hello"));
    EXPECT_TRUE(PgVerifyMbstr(PgEncoding::kSqlAscii, "\xFF\xFE"));
}

TEST(MbutilsTest, VerifyMbstrUtf8Valid) {
    EXPECT_TRUE(PgVerifyMbstr(PgEncoding::kUtf8, "hello"));
    EXPECT_TRUE(PgVerifyMbstr(PgEncoding::kUtf8, "héllo"));
    EXPECT_TRUE(PgVerifyMbstr(PgEncoding::kUtf8, "中文"));
    EXPECT_TRUE(PgVerifyMbstr(PgEncoding::kUtf8, "😀"));
}

TEST(MbutilsTest, VerifyMbstrUtf8InvalidContinuation) {
    // Unexpected continuation byte.
    EXPECT_FALSE(PgVerifyMbstr(PgEncoding::kUtf8, std::string("\x80")));
    EXPECT_FALSE(PgVerifyMbstr(PgEncoding::kUtf8, std::string("A\x80")));
}

TEST(MbutilsTest, VerifyMbstrUtf8Truncated) {
    // Truncated 2-byte sequence.
    EXPECT_FALSE(PgVerifyMbstr(PgEncoding::kUtf8, std::string("\xC3")));
    // Truncated 3-byte sequence.
    EXPECT_FALSE(PgVerifyMbstr(PgEncoding::kUtf8, std::string("\xE4\xB8")));
    // Truncated 4-byte sequence.
    EXPECT_FALSE(PgVerifyMbstr(PgEncoding::kUtf8, std::string("\xF0\x9F\x98")));
}

TEST(MbutilsTest, VerifyMbstrUtf8BadContinuation) {
    // 2-byte sequence with non-continuation second byte.
    EXPECT_FALSE(PgVerifyMbstr(PgEncoding::kUtf8, std::string("\xC3\x41")));
}

TEST(MbutilsTest, VerifyMbstrLatin1) {
    // All byte sequences are valid for single-byte encodings.
    EXPECT_TRUE(PgVerifyMbstr(PgEncoding::kLatin1, "hello"));
    EXPECT_TRUE(PgVerifyMbstr(PgEncoding::kLatin1, "\xFF\xFE"));
}

// ===========================================================================
// PgValidServerEncoding
// ===========================================================================

TEST(MbutilsTest, ValidServerEncoding) {
    EXPECT_TRUE(PgValidServerEncoding(PgEncoding::kUtf8));
    EXPECT_TRUE(PgValidServerEncoding(PgEncoding::kLatin1));
    EXPECT_FALSE(PgValidServerEncoding(PgEncoding::kSjis));
}

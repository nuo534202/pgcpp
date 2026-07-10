// encoding_test.cpp — Unit tests for character encoding metadata (P2-6).
//
// Tests encoding ID lookup, name-to-encoding resolution, and encoding
// metadata (max_mblen, is_server_encoding).

#include "utils/mb/encoding.hpp"

#include <gtest/gtest.h>

#include <string_view>

using pgcpp::utils::GetEncodingByName;
using pgcpp::utils::GetEncodingInfo;
using pgcpp::utils::IsValidServerEncoding;
using pgcpp::utils::PgCharToEncoding;
using pgcpp::utils::PgEncoding;
using pgcpp::utils::PgEncodingToChar;

// ===========================================================================
// EncodingInfo lookup
// ===========================================================================

TEST(EncodingTest, GetEncodingInfoUtf8) {
    const auto* info = GetEncodingInfo(PgEncoding::kUtf8);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->encoding, PgEncoding::kUtf8);
    EXPECT_STREQ(info->name, "UTF8");
    EXPECT_EQ(info->max_mblen, 4);
    EXPECT_TRUE(info->is_server_encoding);
}

TEST(EncodingTest, GetEncodingInfoSqlAscii) {
    const auto* info = GetEncodingInfo(PgEncoding::kSqlAscii);
    ASSERT_NE(info, nullptr);
    EXPECT_STREQ(info->name, "SQL_ASCII");
    EXPECT_EQ(info->max_mblen, 1);
    EXPECT_TRUE(info->is_server_encoding);
}

TEST(EncodingTest, GetEncodingInfoLatin1) {
    const auto* info = GetEncodingInfo(PgEncoding::kLatin1);
    ASSERT_NE(info, nullptr);
    EXPECT_STREQ(info->name, "LATIN1");
    EXPECT_EQ(info->max_mblen, 1);
    EXPECT_TRUE(info->is_server_encoding);
}

TEST(EncodingTest, GetEncodingInfoGbk) {
    const auto* info = GetEncodingInfo(PgEncoding::kGbk);
    ASSERT_NE(info, nullptr);
    EXPECT_STREQ(info->name, "GBK");
    EXPECT_EQ(info->max_mblen, 2);
    EXPECT_TRUE(info->is_server_encoding);
}

TEST(EncodingTest, GetEncodingInfoGb18030) {
    const auto* info = GetEncodingInfo(PgEncoding::kGb18030);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->max_mblen, 4);
}

// ===========================================================================
// Name lookup
// ===========================================================================

TEST(EncodingTest, GetEncodingByNameCanonical) {
    EXPECT_EQ(GetEncodingByName("UTF8"), PgEncoding::kUtf8);
    EXPECT_EQ(GetEncodingByName("SQL_ASCII"), PgEncoding::kSqlAscii);
    EXPECT_EQ(GetEncodingByName("LATIN1"), PgEncoding::kLatin1);
    EXPECT_EQ(GetEncodingByName("GBK"), PgEncoding::kGbk);
    EXPECT_EQ(GetEncodingByName("GB18030"), PgEncoding::kGb18030);
}

TEST(EncodingTest, GetEncodingByNameAlias) {
    EXPECT_EQ(GetEncodingByName("UTF-8"), PgEncoding::kUtf8);
    EXPECT_EQ(GetEncodingByName("ASCII"), PgEncoding::kSqlAscii);
    EXPECT_EQ(GetEncodingByName("ISO-8859-1"), PgEncoding::kLatin1);
}

TEST(EncodingTest, GetEncodingByNameCaseInsensitive) {
    EXPECT_EQ(GetEncodingByName("utf8"), PgEncoding::kUtf8);
    EXPECT_EQ(GetEncodingByName("utf-8"), PgEncoding::kUtf8);
    EXPECT_EQ(GetEncodingByName("Utf8"), PgEncoding::kUtf8);
    EXPECT_EQ(GetEncodingByName("latin1"), PgEncoding::kLatin1);
    EXPECT_EQ(GetEncodingByName("Latin1"), PgEncoding::kLatin1);
}

TEST(EncodingTest, GetEncodingByNameNotFound) {
    // Unknown encoding falls back to SQL_ASCII (matching PG behavior).
    EXPECT_EQ(GetEncodingByName("NONEXISTENT"), PgEncoding::kSqlAscii);
    EXPECT_EQ(GetEncodingByName(""), PgEncoding::kSqlAscii);
}

TEST(EncodingTest, PgCharToEncodingIsAlias) {
    EXPECT_EQ(PgCharToEncoding("UTF8"), GetEncodingByName("UTF8"));
    EXPECT_EQ(PgCharToEncoding("LATIN1"), GetEncodingByName("LATIN1"));
}

// ===========================================================================
// PgEncodingToChar
// ===========================================================================

TEST(EncodingTest, PgEncodingToCharBasic) {
    EXPECT_STREQ(PgEncodingToChar(PgEncoding::kUtf8), "UTF8");
    EXPECT_STREQ(PgEncodingToChar(PgEncoding::kSqlAscii), "SQL_ASCII");
    EXPECT_STREQ(PgEncodingToChar(PgEncoding::kLatin1), "LATIN1");
    EXPECT_STREQ(PgEncodingToChar(PgEncoding::kGbk), "GBK");
}

// ===========================================================================
// IsValidServerEncoding
// ===========================================================================

TEST(EncodingTest, IsValidServerEncoding) {
    EXPECT_TRUE(IsValidServerEncoding(PgEncoding::kUtf8));
    EXPECT_TRUE(IsValidServerEncoding(PgEncoding::kSqlAscii));
    EXPECT_TRUE(IsValidServerEncoding(PgEncoding::kLatin1));
    EXPECT_TRUE(IsValidServerEncoding(PgEncoding::kGbk));
    // SJIS is not a valid server encoding in pgcpp.
    EXPECT_FALSE(IsValidServerEncoding(PgEncoding::kSjis));
}

// ===========================================================================
// Round-trip: name -> encoding -> name
// ===========================================================================

TEST(EncodingTest, RoundTripNameToEncodingToName) {
    for (const auto& name : {"UTF8", "SQL_ASCII", "LATIN1", "GBK", "GB18030", "BIG5"}) {
        PgEncoding enc = GetEncodingByName(name);
        EXPECT_STREQ(PgEncodingToChar(enc), name) << "Round-trip failed for " << name;
    }
}

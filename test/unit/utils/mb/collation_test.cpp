// collation_test.cpp — Unit tests for collation-aware comparison (P2-6).
//
// Tests VarStrCmpC (byte comparison), VarStrCmpCodepoint (Unicode
// codepoint comparison), and VarStrCmp (collation-aware dispatch).

#include "utils/mb/collation.hpp"

#include <gtest/gtest.h>

#include <string>

#include "catalog/catalog.hpp"
#include "catalog/pg_collation.hpp"
#include "utils/mb/mbutils.hpp"

using pgcpp::catalog::CollProvider;
using pgcpp::catalog::kC_COLLATION_OID;
using pgcpp::catalog::kDefaultCollationOid;
using pgcpp::catalog::kInvalidOid;
using pgcpp::catalog::kPOSIX_COLLATION_OID;
using pgcpp::catalog::Oid;
using pgcpp::utils::GetCollationProvider;
using pgcpp::utils::PgEncoding;
using pgcpp::utils::SetDatabaseEncoding;
using pgcpp::utils::VarStrCmp;
using pgcpp::utils::VarStrCmpC;
using pgcpp::utils::VarStrCmpCodepoint;

// ===========================================================================
// VarStrCmpC — byte-by-byte comparison (C/POSIX collation)
// ===========================================================================

TEST(CollationTest, VarStrCmpCEqual) {
    EXPECT_EQ(VarStrCmpC("abc", "abc"), 0);
    EXPECT_EQ(VarStrCmpC("", ""), 0);
    EXPECT_EQ(VarStrCmpC("a", "a"), 0);
}

TEST(CollationTest, VarStrCmpCLessThan) {
    EXPECT_LT(VarStrCmpC("abc", "abd"), 0);
    EXPECT_LT(VarStrCmpC("a", "b"), 0);
    EXPECT_LT(VarStrCmpC("", "a"), 0);
    EXPECT_LT(VarStrCmpC("ab", "abc"), 0);  // shorter prefix sorts first
}

TEST(CollationTest, VarStrCmpCGreaterThan) {
    EXPECT_GT(VarStrCmpC("abd", "abc"), 0);
    EXPECT_GT(VarStrCmpC("b", "a"), 0);
    EXPECT_GT(VarStrCmpC("a", ""), 0);
    EXPECT_GT(VarStrCmpC("abc", "ab"), 0);
}

// ===========================================================================
// VarStrCmpCodepoint — Unicode codepoint comparison
// ===========================================================================

TEST(CollationTest, VarStrCmpCodepointAscii) {
    // For ASCII, codepoint comparison = byte comparison.
    EXPECT_EQ(VarStrCmpCodepoint("abc", "abc", PgEncoding::kUtf8), 0);
    EXPECT_LT(VarStrCmpCodepoint("abc", "abd", PgEncoding::kUtf8), 0);
    EXPECT_GT(VarStrCmpCodepoint("abd", "abc", PgEncoding::kUtf8), 0);
}

TEST(CollationTest, VarStrCmpCodepointUtf8) {
    // Compare strings with multi-byte UTF-8 characters.
    // "a" (U+0061) < "é" (U+00E9) < "中" (U+4E2D)
    EXPECT_LT(VarStrCmpCodepoint("a", "é", PgEncoding::kUtf8), 0);
    EXPECT_GT(VarStrCmpCodepoint("é", "a", PgEncoding::kUtf8), 0);
    EXPECT_LT(VarStrCmpCodepoint("é", "中", PgEncoding::kUtf8), 0);
}

TEST(CollationTest, VarStrCmpCodepointSameString) {
    EXPECT_EQ(VarStrCmpCodepoint("中文", "中文", PgEncoding::kUtf8), 0);
}

TEST(CollationTest, VarStrCmpCodepointDifferentLengths) {
    // "abc" < "abcd" (common prefix, shorter sorts first)
    EXPECT_LT(VarStrCmpCodepoint("abc", "abcd", PgEncoding::kUtf8), 0);
    EXPECT_GT(VarStrCmpCodepoint("abcd", "abc", PgEncoding::kUtf8), 0);
}

TEST(CollationTest, VarStrCmpCodepointNonUtf8) {
    // For non-UTF-8, falls back to byte comparison.
    EXPECT_EQ(VarStrCmpCodepoint("abc", "abc", PgEncoding::kLatin1), 0);
    EXPECT_LT(VarStrCmpCodepoint("abc", "abd", PgEncoding::kLatin1), 0);
}

TEST(CollationTest, VarStrCmpCodepointEmoji) {
    // 😀 (U+1F600) > 中 (U+4E2D) > é (U+00E9) > a (U+0061)
    EXPECT_GT(VarStrCmpCodepoint("😀", "中", PgEncoding::kUtf8), 0);
    EXPECT_GT(VarStrCmpCodepoint("中", "é", PgEncoding::kUtf8), 0);
    EXPECT_GT(VarStrCmpCodepoint("é", "a", PgEncoding::kUtf8), 0);
}

// ===========================================================================
// VarStrCmp — collation-aware dispatch
// ===========================================================================

TEST(CollationTest, VarStrCmpInvalidOid) {
    // Invalid OID uses byte comparison.
    EXPECT_EQ(VarStrCmp("abc", "abc", kInvalidOid), 0);
    EXPECT_LT(VarStrCmp("abc", "abd", kInvalidOid), 0);
}

TEST(CollationTest, VarStrCmpDefaultOid) {
    // Default collation uses byte comparison.
    EXPECT_EQ(VarStrCmp("abc", "abc", kDefaultCollationOid), 0);
    EXPECT_LT(VarStrCmp("abc", "abd", kDefaultCollationOid), 0);
}

TEST(CollationTest, VarStrCmpCOid) {
    // C collation uses byte comparison.
    EXPECT_EQ(VarStrCmp("abc", "abc", kC_COLLATION_OID), 0);
    EXPECT_LT(VarStrCmp("abc", "abd", kC_COLLATION_OID), 0);
}

TEST(CollationTest, VarStrCmpPosixOid) {
    // POSIX collation uses byte comparison.
    EXPECT_EQ(VarStrCmp("abc", "abc", kPOSIX_COLLATION_OID), 0);
    EXPECT_LT(VarStrCmp("abc", "abd", kPOSIX_COLLATION_OID), 0);
}

// ===========================================================================
// GetCollationProvider
// ===========================================================================

TEST(CollationTest, GetCollationProviderInvalid) {
    EXPECT_EQ(GetCollationProvider(kInvalidOid), CollProvider::kDefault);
}

TEST(CollationTest, GetCollationProviderDefault) {
    EXPECT_EQ(GetCollationProvider(kDefaultCollationOid), CollProvider::kDefault);
}

TEST(CollationTest, GetCollationProviderNoCatalog) {
    // With no catalog set, unknown collation OID returns default provider.
    // (Well-known OIDs C/POSIX are handled before catalog lookup.)
    Oid unknown_oid = 99999;
    EXPECT_EQ(GetCollationProvider(unknown_oid), CollProvider::kDefault);
}

// ===========================================================================
// VarStrCmp with UTF-8 multibyte strings
// ===========================================================================

TEST(CollationTest, VarStrCmpUtf8WithDefaultCollation) {
    // Default collation: byte comparison.
    // In UTF-8, "é" = 0xC3 0xA9, which is > "a" (0x61) at byte level.
    SetDatabaseEncoding(PgEncoding::kUtf8);
    EXPECT_LT(VarStrCmp("a", "é", kDefaultCollationOid), 0);
    EXPECT_GT(VarStrCmp("é", "a", kDefaultCollationOid), 0);
}

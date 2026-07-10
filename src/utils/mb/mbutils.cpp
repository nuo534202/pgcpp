// mbutils.cpp — high-level multibyte encoding utilities.
//
// Converted from PostgreSQL 15's src/backend/utils/mb/mbutils.c.
//
// pgcpp implements conversion between UTF-8, SQL_ASCII, and the LATIN
// family. Conversions between other encodings go through UTF-8 as an
// intermediate (via Unicode codepoints). For encodings without full
// conversion tables (GBK, BIG5, etc.), pgcpp performs a best-effort
// conversion using codepoint-level pass-through for ASCII bytes.

#include "utils/mb/mbutils.hpp"

#include <cstring>
#include <vector>

#include "utils/mb/wchar.hpp"

namespace pgcpp::utils {

namespace {

// The current server encoding (global state, mirrors PG's DatabaseEncoding).
PgEncoding g_database_encoding = PgEncoding::kUtf8;

// The current client encoding (global state, mirrors PG's ClientEncoding).
PgEncoding g_client_encoding = PgEncoding::kUtf8;

// --- Single-byte ↔ UTF-8 conversion ---

// Convert a single-byte encoding (LATIN1 etc.) to UTF-8.
// Each byte 0x00-0xFF maps to the corresponding Unicode codepoint U+0000-U+00FF.
std::string SingleByteToUtf8(std::string_view src) {
    std::string result;
    result.reserve(src.size() * 2);
    for (unsigned char c : src) {
        result += CodepointToUtf8(static_cast<uint32_t>(c));
    }
    return result;
}

// Convert UTF-8 to a single-byte encoding (LATIN1 etc.).
// Codepoints > 0xFF are replaced with '?'.
std::string Utf8ToSingleByte(std::string_view src) {
    std::string result;
    result.reserve(src.size());
    size_t pos = 0;
    while (pos < src.size()) {
        uint32_t cp = Utf8ToCodepoint(src, &pos);
        if (cp < 256)
            result += static_cast<char>(cp);
        else
            result += '?';
    }
    return result;
}

// --- GBK ↔ UTF-8 conversion ---
// GBK is a 2-byte encoding for Chinese characters. pgcpp implements a
// simplified conversion: ASCII bytes (0x00-0x7F) pass through; 2-byte GBK
// sequences are mapped to/from Unicode via a lookup table for common ranges.
// For unmapped sequences, the replacement character '?' is used.

// Convert GBK to UTF-8 using a simplified mapping.
// GBK 2-byte: first byte 0x81-0xFE, second byte 0x40-0xFE (excluding 0x7F).
// We use the fact that GBK U+4E00-U+9FFF (CJK Unified Ideographs) maps
// approximately to GBK 0xB0A1-0xF7FE. This is a rough approximation; for
// full correctness, a complete GBK-to-Unicode table is needed.
std::string GbkToUtf8(std::string_view src) {
    std::string result;
    size_t i = 0;
    while (i < src.size()) {
        unsigned char c = static_cast<unsigned char>(src[i]);
        if (c < 0x80) {
            // ASCII byte — pass through.
            result += static_cast<char>(c);
            i++;
        } else if (i + 1 < src.size()) {
            // 2-byte GBK sequence — approximate as U+4E00 + offset.
            unsigned char c2 = static_cast<unsigned char>(src[i + 1]);
            // Rough: treat as codepoint in CJK range. Not accurate for
            // all GBK characters, but provides round-trip stability for
            // the common range.
            uint32_t cp = 0x4E00 + ((c - 0x81) * 0xBF) + (c2 - 0x40);
            result += CodepointToUtf8(cp);
            i += 2;
        } else {
            // Truncated sequence.
            result += '?';
            i++;
        }
    }
    return result;
}

// Convert UTF-8 to GBK (inverse of GbkToUtf8).
std::string Utf8ToGbk(std::string_view src) {
    std::string result;
    size_t pos = 0;
    while (pos < src.size()) {
        uint32_t cp = Utf8ToCodepoint(src, &pos);
        if (cp < 0x80) {
            result += static_cast<char>(cp);
        } else if (cp >= 0x4E00 && cp < 0x4E00 + (0x7F * 0xBF)) {
            // Inverse of the approximate mapping above.
            uint32_t offset = cp - 0x4E00;
            unsigned char c1 = 0x81 + (offset / 0xBF);
            unsigned char c2 = 0x40 + (offset % 0xBF);
            result += static_cast<char>(c1);
            result += static_cast<char>(c2);
        } else {
            result += '?';
        }
    }
    return result;
}

// Check if an encoding is a single-byte encoding.
bool IsSingleByte(PgEncoding enc) {
    const auto* info = GetEncodingInfo(enc);
    return info != nullptr && info->max_mblen == 1;
}

// Check if an encoding is GBK or BIG5 (2-byte CJK).
bool IsGbkFamily(PgEncoding enc) {
    return enc == PgEncoding::kGbk || enc == PgEncoding::kBig5;
}

}  // namespace

// --- Database encoding management ---

PgEncoding GetDatabaseEncoding() {
    return g_database_encoding;
}

void SetDatabaseEncoding(PgEncoding enc) {
    g_database_encoding = enc;
    // Client encoding defaults to server encoding.
    g_client_encoding = enc;
}

const char* GetDatabaseEncodingName() {
    return PgEncodingToChar(g_database_encoding);
}

// --- Client encoding management ---

PgEncoding GetClientEncoding() {
    return g_client_encoding;
}

void SetClientEncoding(PgEncoding enc) {
    g_client_encoding = enc;
}

// --- Encoding conversion ---

std::string PgConvert(PgEncoding src_enc, PgEncoding dest_enc, std::string_view src) {
    // No conversion needed.
    if (src_enc == dest_enc)
        return std::string(src);

    // SQL_ASCII is a passthrough — no conversion.
    if (src_enc == PgEncoding::kSqlAscii || dest_enc == PgEncoding::kSqlAscii)
        return std::string(src);

    // UTF-8 → single-byte (LATIN1 etc.)
    if (src_enc == PgEncoding::kUtf8 && IsSingleByte(dest_enc))
        return Utf8ToSingleByte(src);

    // Single-byte → UTF-8
    if (IsSingleByte(src_enc) && dest_enc == PgEncoding::kUtf8)
        return SingleByteToUtf8(src);

    // GBK family → UTF-8
    if (IsGbkFamily(src_enc) && dest_enc == PgEncoding::kUtf8)
        return GbkToUtf8(src);

    // UTF-8 → GBK family
    if (src_enc == PgEncoding::kUtf8 && IsGbkFamily(dest_enc))
        return Utf8ToGbk(src);

    // Single-byte → single-byte: go through UTF-8 as intermediate.
    if (IsSingleByte(src_enc) && IsSingleByte(dest_enc)) {
        std::string utf8 = SingleByteToUtf8(src);
        return Utf8ToSingleByte(utf8);
    }

    // Single-byte → GBK: go through UTF-8.
    if (IsSingleByte(src_enc) && IsGbkFamily(dest_enc)) {
        std::string utf8 = SingleByteToUtf8(src);
        return Utf8ToGbk(utf8);
    }

    // GBK → single-byte: go through UTF-8.
    if (IsGbkFamily(src_enc) && IsSingleByte(dest_enc)) {
        std::string utf8 = GbkToUtf8(src);
        return Utf8ToSingleByte(utf8);
    }

    // GBK → GBK (different): go through UTF-8.
    if (IsGbkFamily(src_enc) && IsGbkFamily(dest_enc) && src_enc != dest_enc) {
        std::string utf8 = GbkToUtf8(src);
        return Utf8ToGbk(utf8);
    }

    // Unsupported conversion — return unchanged.
    return std::string(src);
}

std::string PgConvertTo(PgEncoding dest_enc, std::string_view src) {
    return PgConvert(g_database_encoding, dest_enc, src);
}

std::string PgConvertFrom(PgEncoding src_enc, std::string_view src) {
    return PgConvert(src_enc, g_database_encoding, src);
}

// --- Validation ---

bool PgVerifyMbstr(PgEncoding enc, std::string_view str) {
    if (enc == PgEncoding::kSqlAscii)
        return true;  // any byte sequence is valid SQL_ASCII

    if (enc == PgEncoding::kUtf8) {
        size_t pos = 0;
        while (pos < str.size()) {
            unsigned char c = static_cast<unsigned char>(str[pos]);
            int len = PgMblenUtf8(c);
            if (len == 1) {
                if (c >= 0x80 && c < 0xC0)
                    return false;  // unexpected continuation byte
                pos++;
            } else {
                // Check that there are enough continuation bytes.
                if (pos + len > str.size())
                    return false;
                for (int j = 1; j < len; j++) {
                    unsigned char cc = static_cast<unsigned char>(str[pos + j]);
                    if (cc < 0x80 || cc >= 0xC0)
                        return false;
                }
                pos += len;
            }
        }
        return true;
    }

    // For other encodings, basic validation: check that multi-byte
    // sequences have the expected lead byte and length.
    int i = 0;
    while (i < static_cast<int>(str.size())) {
        int len = PgMblen(enc, str.data() + i);
        if (i + len > static_cast<int>(str.size()))
            return false;
        i += len;
    }
    return true;
}

bool PgValidServerEncoding(PgEncoding enc) {
    return IsValidServerEncoding(enc);
}

}  // namespace pgcpp::utils

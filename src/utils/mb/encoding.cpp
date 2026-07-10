// encoding.cpp — encoding metadata and name lookup.
//
// Converted from PostgreSQL 15's src/backend/utils/mb/encnames.c.

#include "utils/mb/encoding.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <string>

namespace pgcpp::utils {

namespace {

// The encoding table. IDs match PostgreSQL's pg_enc enum values.
// Only encodings relevant to pgcpp are fully described; others are
// listed for ID compatibility but marked as non-server encodings.
constexpr std::array<EncodingInfo, 42> kEncodingTable = {{
    {PgEncoding::kSqlAscii, "SQL_ASCII", "ASCII", 1, true},
    {PgEncoding::kEucJp, "EUC_JP", "EUC_JP", 3, true},
    {PgEncoding::kEucCn, "EUC_CN", "EUC_CN", 2, true},
    {PgEncoding::kEucKr, "EUC_KR", "EUC_KR", 2, true},
    {PgEncoding::kEucTw, "EUC_TW", "EUC_TW", 4, true},
    {PgEncoding::kEucJis2004, "EUC_JIS_2004", "EUC_JIS_2004", 3, true},
    {PgEncoding::kUtf8, "UTF8", "UTF-8", 4, true},
    {PgEncoding::kMuleInternal, "MULE_INTERNAL", "MULE_INTERNAL", 4, true},
    {PgEncoding::kLatin1, "LATIN1", "ISO-8859-1", 1, true},
    {PgEncoding::kLatin2, "LATIN2", "ISO-8859-2", 1, true},
    {PgEncoding::kLatin3, "LATIN3", "ISO-8859-3", 1, true},
    {PgEncoding::kLatin4, "LATIN4", "ISO-8859-4", 1, true},
    {PgEncoding::kLatin5, "LATIN5", "ISO-8859-9", 1, true},
    {PgEncoding::kLatin6, "LATIN6", "ISO-8859-10", 1, true},
    {PgEncoding::kLatin7, "LATIN7", "ISO-8859-13", 1, true},
    {PgEncoding::kLatin8, "LATIN8", "ISO-8859-14", 1, true},
    {PgEncoding::kLatin9, "LATIN9", "ISO-8859-15", 1, true},
    {PgEncoding::kLatin10, "LATIN10", "ISO-8859-16", 1, true},
    {PgEncoding::kWin1256, "WIN1256", "CP1256", 1, true},
    {PgEncoding::kWin1258, "WIN1258", "CP1258", 1, true},
    {PgEncoding::kWin866, "WIN866", "CP866", 1, true},
    {PgEncoding::kWin874, "WIN874", "CP874", 1, true},
    {PgEncoding::kKoi8r, "KOI8R", "KOI8-R", 1, true},
    {PgEncoding::kWin1251, "WIN1251", "CP1251", 1, true},
    {PgEncoding::kWin1252, "WIN1252", "CP1252", 1, true},
    {PgEncoding::kIso8859_5, "ISO_8859_5", "ISO-8859-5", 1, true},
    {PgEncoding::kIso8859_6, "ISO_8859_6", "ISO-8859-6", 1, true},
    {PgEncoding::kIso8859_7, "ISO_8859_7", "ISO-8859-7", 1, true},
    {PgEncoding::kIso8859_8, "ISO_8859_8", "ISO-8859-8", 1, true},
    {PgEncoding::kWin1250, "WIN1250", "CP1250", 1, true},
    {PgEncoding::kWin1253, "WIN1253", "CP1253", 1, true},
    {PgEncoding::kWin1254, "WIN1254", "CP1254", 1, true},
    {PgEncoding::kWin1255, "WIN1255", "CP1255", 1, true},
    {PgEncoding::kWin1257, "WIN1257", "CP1257", 1, true},
    {PgEncoding::kKoi8u, "KOI8U", "KOI8-U", 1, true},
    {PgEncoding::kSjis, "SJIS", "SHIFT-JIS", 2, false},
    {PgEncoding::kBig5, "BIG5", "BIG5", 2, true},
    {PgEncoding::kGbk, "GBK", "GBK", 2, true},
    {PgEncoding::kUhc, "UHC", "UHC", 2, true},
    {PgEncoding::kGb18030, "GB18030", "GB18030", 4, true},
    {PgEncoding::kJohab, "JOHAB", "JOHAB", 3, false},
    {PgEncoding::kShiftJis2004, "SHIFT_JIS_2004", "SHIFT-JIS-2004", 2, false},
}};

// Case-insensitive string comparison for encoding name matching.
bool IEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

}  // namespace

const EncodingInfo* GetEncodingInfo(PgEncoding enc) {
    for (const auto& info : kEncodingTable) {
        if (info.encoding == enc)
            return &info;
    }
    return nullptr;
}

PgEncoding GetEncodingByName(std::string_view name) {
    for (const auto& info : kEncodingTable) {
        if (IEquals(name, info.name) || IEquals(name, info.alias))
            return info.encoding;
    }
    return PgEncoding::kSqlAscii;
}

const char* PgEncodingToChar(PgEncoding enc) {
    const auto* info = GetEncodingInfo(enc);
    return info != nullptr ? info->name : "SQL_ASCII";
}

PgEncoding PgCharToEncoding(std::string_view name) {
    return GetEncodingByName(name);
}

bool IsValidServerEncoding(PgEncoding enc) {
    const auto* info = GetEncodingInfo(enc);
    return info != nullptr && info->is_server_encoding;
}

}  // namespace pgcpp::utils

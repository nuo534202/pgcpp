// encoding.hpp — PostgreSQL character encoding IDs and metadata.
//
// Converted from PostgreSQL 15's src/include/mb/pg_wchar.h.
//
// Each database has a single server encoding. Client connections may use a
// different encoding, with conversion happening at the protocol boundary.
// pgcpp implements a subset of PostgreSQL's encodings, focusing on UTF-8
// (the default), SQL_ASCII, LATIN1-LATIN10, GBK, BIG5, and GB18030.

#pragma once

#include <cstdint>
#include <string_view>

namespace pgcpp::utils {

// PgEncoding — mirrors PostgreSQL's pg_enc enum.
// IDs match PostgreSQL's encoding IDs for compatibility.
enum class PgEncoding : int {
    kSqlAscii = 0,    // SQL_ASCII
    kEucJp = 1,       // EUC_JP
    kEucCn = 2,       // EUC_CN
    kEucKr = 3,       // EUC_KR
    kEucTw = 4,       // EUC_TW
    kEucJis2004 = 5,  // EUC_JIS_2004
    kUtf8 = 6,        // UTF8
    kMuleInternal = 7,
    kLatin1 = 8,    // LATIN1
    kLatin2 = 9,    // LATIN2
    kLatin3 = 10,   // LATIN3
    kLatin4 = 11,   // LATIN4
    kLatin5 = 12,   // LATIN5
    kLatin6 = 13,   // LATIN6
    kLatin7 = 14,   // LATIN7
    kLatin8 = 15,   // LATIN8
    kLatin9 = 16,   // LATIN9
    kLatin10 = 17,  // LATIN10
    kWin1256 = 18,
    kWin1258 = 19,
    kWin866 = 20,
    kWin874 = 21,
    kKoi8r = 22,  // KOI8R
    kWin1251 = 23,
    kWin1252 = 24,
    kIso8859_5 = 25,
    kIso8859_6 = 26,
    kIso8859_7 = 27,
    kIso8859_8 = 28,
    kWin1250 = 29,
    kWin1253 = 30,
    kWin1254 = 31,
    kWin1255 = 32,
    kWin1257 = 33,
    kKoi8u = 34,
    kSjis = 35,
    kBig5 = 36,  // BIG5
    kGbk = 37,   // GBK
    kUhc = 38,
    kGb18030 = 39,  // GB18030
    kJohab = 40,
    kShiftJis2004 = 41,
};

// EncodingInfo — metadata for a character encoding.
struct EncodingInfo {
    PgEncoding encoding;
    const char* name;         // canonical name (e.g. "UTF8")
    const char* alias;        // common alias (e.g. "UTF-8")
    int max_mblen;            // maximum bytes per character
    bool is_server_encoding;  // can be used as server encoding?
};

// GetEncodingInfo — return metadata for an encoding.
const EncodingInfo* GetEncodingInfo(PgEncoding enc);

// GetEncodingByName — look up an encoding by canonical name or alias.
// Case-insensitive. Returns kSqlAscii if not found (matching PG behavior
// of falling back to SQL_ASCII).
PgEncoding GetEncodingByName(std::string_view name);

// PgEncodingToChar — return the canonical name for an encoding.
const char* PgEncodingToChar(PgEncoding enc);

// PgCharToEncoding — return the encoding for a name (alias for
// GetEncodingByName, matches PG's pg_char_to_encoding function name).
PgEncoding PgCharToEncoding(std::string_view name);

// IsValidServerEncoding — true if the encoding can be used as the server
// encoding (all pgcpp-supported encodings are valid).
bool IsValidServerEncoding(PgEncoding enc);

}  // namespace pgcpp::utils

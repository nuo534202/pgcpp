// wchar.cpp — multibyte character width and wide-char conversion.
//
// Converted from PostgreSQL 15's src/backend/utils/mb/wchar.c.
//
// UTF-8 encoding rules:
//   0xxxxxxx                              → 1 byte  (U+0000..U+007F)
//   110xxxxx 10xxxxxx                     → 2 bytes (U+0080..U+07FF)
//   1110xxxx 10xxxxxx 10xxxxxx            → 3 bytes (U+0800..U+FFFF)
//   11110xxx 10xxxxxx 10xxxxxx 10xxxxxx   → 4 bytes (U+10000..U+10FFFF)

#include "utils/mb/wchar.hpp"

#include <cstring>

namespace pgcpp::utils {

int PgMblenUtf8(unsigned char c) {
    if (c < 0x80)
        return 1;
    if (c < 0xC0)
        return 1;  // continuation byte — treat as 1
    if (c < 0xE0)
        return 2;
    if (c < 0xF0)
        return 3;
    if (c < 0xF8)
        return 4;
    return 1;  // invalid leading byte
}

int PgMblen(PgEncoding enc, const char* mbstr) {
    if (mbstr == nullptr || mbstr[0] == '\0')
        return 0;

    unsigned char c = static_cast<unsigned char>(mbstr[0]);

    switch (enc) {
        case PgEncoding::kUtf8:
            return PgMblenUtf8(c);
        case PgEncoding::kGbk:
        case PgEncoding::kBig5:
        case PgEncoding::kUhc:
        case PgEncoding::kSjis:
            // 2-byte encodings: if lead byte >= 0x80, it's 2 bytes.
            return (c >= 0x80) ? 2 : 1;
        case PgEncoding::kGb18030:
            // GB18030: 1, 2, or 4 bytes. Simplified: 2 bytes if >= 0x81.
            return (c >= 0x81) ? 2 : 1;
        case PgEncoding::kEucJp:
        case PgEncoding::kEucJis2004:
            // EUC_JP: 1-3 bytes. Simplified: 2 bytes if >= 0xA1, 3 if 0x8F.
            if (c == 0x8F)
                return 3;
            return (c >= 0xA1) ? 2 : 1;
        case PgEncoding::kEucCn:
        case PgEncoding::kEucKr:
        case PgEncoding::kEucTw:
            return (c >= 0xA1) ? 2 : 1;
        default:
            // All single-byte encodings (SQL_ASCII, LATIN1-10, WIN*, KOI8*, etc.)
            return 1;
    }
}

int PgDsplen(PgEncoding enc, const char* mbstr) {
    if (mbstr == nullptr || mbstr[0] == '\0')
        return 0;

    // Simplified model: all characters have display width 1.
    // (PostgreSQL uses wcwidth() for precise widths; pgcpp keeps it simple.)
    (void)enc;
    return 1;
}

int PgMbstrlen(PgEncoding enc, const char* mbstr) {
    if (mbstr == nullptr)
        return 0;
    return PgMbstrlenWithLen(enc, mbstr, static_cast<int>(std::strlen(mbstr)));
}

int PgMbstrlenWithLen(PgEncoding enc, const char* mbstr, int limit) {
    if (mbstr == nullptr || limit <= 0)
        return 0;

    int count = 0;
    int i = 0;
    while (i < limit) {
        int len = PgMblen(enc, mbstr + i);
        if (len <= 0)
            len = 1;
        i += len;
        count++;
    }
    return count;
}

uint32_t Utf8ToCodepoint(std::string_view str, size_t* pos) {
    if (*pos >= str.size())
        return 0;

    unsigned char c = static_cast<unsigned char>(str[*pos]);
    *pos += 1;

    // 1-byte: 0xxxxxxx
    if (c < 0x80)
        return c;

    // 2-byte: 110xxxxx 10xxxxxx
    if (c < 0xE0) {
        uint32_t cp = (c & 0x1F) << 6;
        if (*pos < str.size()) {
            cp |= static_cast<unsigned char>(str[*pos]) & 0x3F;
            *pos += 1;
        }
        return cp;
    }

    // 3-byte: 1110xxxx 10xxxxxx 10xxxxxx
    if (c < 0xF0) {
        uint32_t cp = (c & 0x0F) << 12;
        if (*pos < str.size()) {
            cp |= (static_cast<unsigned char>(str[*pos]) & 0x3F) << 6;
            *pos += 1;
        }
        if (*pos < str.size()) {
            cp |= static_cast<unsigned char>(str[*pos]) & 0x3F;
            *pos += 1;
        }
        return cp;
    }

    // 4-byte: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
    if (c < 0xF8) {
        uint32_t cp = (c & 0x07) << 18;
        if (*pos < str.size()) {
            cp |= (static_cast<unsigned char>(str[*pos]) & 0x3F) << 12;
            *pos += 1;
        }
        if (*pos < str.size()) {
            cp |= (static_cast<unsigned char>(str[*pos]) & 0x3F) << 6;
            *pos += 1;
        }
        if (*pos < str.size()) {
            cp |= static_cast<unsigned char>(str[*pos]) & 0x3F;
            *pos += 1;
        }
        return cp;
    }

    // Invalid byte — return replacement character.
    return 0xFFFD;
}

std::string CodepointToUtf8(uint32_t cp) {
    std::string result;

    if (cp < 0x80) {
        result += static_cast<char>(cp);
    } else if (cp < 0x800) {
        result += static_cast<char>(0xC0 | (cp >> 6));
        result += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        result += static_cast<char>(0xE0 | (cp >> 12));
        result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        result += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x110000) {
        result += static_cast<char>(0xF0 | (cp >> 18));
        result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        result += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        // Invalid codepoint — emit replacement character.
        result += static_cast<char>(0xEF);
        result += static_cast<char>(0xBF);
        result += static_cast<char>(0xBD);
    }

    return result;
}

int PgMb2wchar(PgEncoding enc, std::string_view mbstr, std::vector<uint32_t>* wchars) {
    wchars->clear();

    if (enc == PgEncoding::kUtf8) {
        size_t pos = 0;
        while (pos < mbstr.size()) {
            wchars->push_back(Utf8ToCodepoint(mbstr, &pos));
        }
    } else {
        // For single-byte encodings, each byte is a codepoint.
        for (char c : mbstr) {
            wchars->push_back(static_cast<unsigned char>(c));
        }
    }

    return static_cast<int>(wchars->size());
}

std::string PgWchar2Mb(PgEncoding enc, const std::vector<uint32_t>& wchars) {
    std::string result;

    if (enc == PgEncoding::kUtf8) {
        for (uint32_t cp : wchars) {
            result += CodepointToUtf8(cp);
        }
    } else {
        // For single-byte encodings, only codepoints < 256 are representable.
        for (uint32_t cp : wchars) {
            if (cp < 256)
                result += static_cast<char>(cp);
            else
                result += '?';
        }
    }

    return result;
}

}  // namespace pgcpp::utils

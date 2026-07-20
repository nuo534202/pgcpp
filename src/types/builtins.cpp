#include "types/builtins.hpp"

#include <cctype>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "common/error/elog.hpp"
#include "common/memory/memory_context.hpp"
#include "types/numutils.hpp"
#include "utils/mb/mbutils.hpp"
#include "utils/mb/wchar.hpp"

namespace pgcpp::types {

using pgcpp::error::LogLevel;
using pgcpp::memory::palloc;

namespace {

// Case-insensitive ASCII string comparison. Returns true if equal.
bool IStringEq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// Allocate a palloc'd C string copy of the given view (null-terminated).
char* PallocCString(std::string_view s) {
    char* buf = static_cast<char*>(palloc(s.size() + 1));
    if (!s.empty()) {
        std::memcpy(buf, s.data(), s.size());
    }
    buf[s.size()] = '\0';
    return buf;
}

// VARHDRSZ — PostgreSQL's varlena header size, used when encoding typmods
// for varlena string types (varchar/bpchar). Matches VARHDRSZ in postgres.h.
constexpr int32_t kVarHdrSz = 4;

// Mbcharcliplen — return the byte length of the first `char_limit` characters
// of the multibyte string `s` (which has `byte_len` bytes). Mirrors
// PostgreSQL's pg_mbcharcliplen(). For single-byte encodings this equals
// min(byte_len, char_limit).
int Mbcharcliplen(const char* s, int byte_len, int char_limit) {
    if (s == nullptr || byte_len <= 0 || char_limit <= 0) {
        return 0;
    }
    pgcpp::utils::PgEncoding enc = pgcpp::utils::GetDatabaseEncoding();
    int clen = 0;  // byte length consumed so far
    int nch = 0;   // character count consumed so far
    int i = 0;
    while (i < byte_len && s[i] != '\0') {
        int ch_len = pgcpp::utils::PgMblen(enc, s + i);
        if (ch_len <= 0) {
            ch_len = 1;
        }
        if (i + ch_len > byte_len) {
            // Truncated multi-byte sequence at end of buffer; stop.
            break;
        }
        ++nch;
        if (nch > char_limit) {
            break;
        }
        clen += ch_len;
        i += ch_len;
    }
    return clen;
}

// Build a varlena text Datum from `data` (byte_len bytes). Mirrors
// PostgreSQL's cstring_to_text_with_len.
Datum MakeTextWithLen(const char* data, std::size_t byte_len) {
    std::size_t total = sizeof(int32_t) + byte_len;
    char* buf = static_cast<char*>(palloc(total));
    int32_t header = static_cast<int32_t>(total);
    std::memcpy(buf, &header, sizeof(header));
    if (byte_len > 0) {
        std::memcpy(buf + sizeof(int32_t), data, byte_len);
    }
    return TextPGetDatum(buf);
}

}  // namespace

// ---------------------------------------------------------------------------
// bool
// ---------------------------------------------------------------------------

Datum bool_in(const char* str) {
    if (str == nullptr) {
        ereport(LogLevel::kError, "invalid input syntax for type boolean: NULL");
    }
    std::string_view s(str);
    if (IStringEq(s, "true") || IStringEq(s, "t") || IStringEq(s, "yes") || IStringEq(s, "y") ||
        IStringEq(s, "on") || s == "1") {
        return BoolGetDatum(true);
    }
    if (IStringEq(s, "false") || IStringEq(s, "f") || IStringEq(s, "no") || IStringEq(s, "n") ||
        IStringEq(s, "off") || s == "0") {
        return BoolGetDatum(false);
    }
    char errbuf[256];
    std::snprintf(errbuf, sizeof(errbuf), "invalid input syntax for type boolean: \"%.*s\"",
                  static_cast<int>(s.size()), s.data());
    ereport(LogLevel::kError, errbuf);
}

char* bool_out(Datum value) {
    return PallocCString(DatumGetBool(value) ? "t" : "f");
}

// ---------------------------------------------------------------------------
// int2 (SMALLINT)
// ---------------------------------------------------------------------------

Datum int2_in(const char* str) {
    if (str == nullptr) {
        ereport(LogLevel::kError, "invalid input syntax for type smallint: NULL");
    }
    errno = 0;
    char* endptr = nullptr;
    long val = std::strtol(str, &endptr, 10);
    if (errno == ERANGE || val < INT16_MIN || val > INT16_MAX) {
        ereport(LogLevel::kError,
                "value \"" + std::string(str) + "\" is out of range for type smallint");
    }
    if (endptr == str) {
        ereport(LogLevel::kError,
                "invalid input syntax for type smallint: \"" + std::string(str) + "\"");
    }
    while (*endptr != '\0' && std::isspace(static_cast<unsigned char>(*endptr))) {
        ++endptr;
    }
    if (*endptr != '\0') {
        ereport(LogLevel::kError,
                "invalid input syntax for type smallint: \"" + std::string(str) + "\"");
    }
    return Int16GetDatum(static_cast<int16_t>(val));
}

char* int2_out(Datum value) {
    return PallocCString(std::to_string(DatumGetInt16(value)));
}

// ---------------------------------------------------------------------------
// int4
// ---------------------------------------------------------------------------

Datum int4_in(const char* str) {
    if (str == nullptr) {
        ereport(LogLevel::kError, "invalid input syntax for type integer: NULL");
    }
    errno = 0;
    char* endptr = nullptr;
    long val = std::strtol(str, &endptr, 10);
    if (errno == ERANGE || val < INT32_MIN || val > INT32_MAX) {
        ereport(LogLevel::kError,
                "value \"" + std::string(str) + "\" is out of range for type integer");
    }
    if (endptr == str) {
        ereport(LogLevel::kError,
                "invalid input syntax for type integer: \"" + std::string(str) + "\"");
    }
    while (*endptr != '\0' && std::isspace(static_cast<unsigned char>(*endptr))) {
        ++endptr;
    }
    if (*endptr != '\0') {
        ereport(LogLevel::kError,
                "invalid input syntax for type integer: \"" + std::string(str) + "\"");
    }
    return Int32GetDatum(static_cast<int32_t>(val));
}

char* int4_out(Datum value) {
    return PallocCString(std::to_string(DatumGetInt32(value)));
}

// ---------------------------------------------------------------------------
// int8
// ---------------------------------------------------------------------------

Datum int8_in(const char* str) {
    if (str == nullptr) {
        ereport(LogLevel::kError, "invalid input syntax for type bigint: NULL");
    }
    errno = 0;
    char* endptr = nullptr;
    long long val = std::strtoll(str, &endptr, 10);
    if (errno == ERANGE || val < INT64_MIN || val > INT64_MAX) {
        ereport(LogLevel::kError,
                "value \"" + std::string(str) + "\" is out of range for type bigint");
    }
    if (endptr == str) {
        ereport(LogLevel::kError,
                "invalid input syntax for type bigint: \"" + std::string(str) + "\"");
    }
    while (*endptr != '\0' && std::isspace(static_cast<unsigned char>(*endptr))) {
        ++endptr;
    }
    if (*endptr != '\0') {
        ereport(LogLevel::kError,
                "invalid input syntax for type bigint: \"" + std::string(str) + "\"");
    }
    return Int64GetDatum(static_cast<int64_t>(val));
}

char* int8_out(Datum value) {
    return PallocCString(std::to_string(DatumGetInt64(value)));
}

// ---------------------------------------------------------------------------
// float8
// ---------------------------------------------------------------------------

Datum float8_in(const char* str) {
    if (str == nullptr) {
        ereport(LogLevel::kError, "invalid input syntax for type double precision: NULL");
    }
    errno = 0;
    char* endptr = nullptr;
    double val = std::strtod(str, &endptr);
    if (errno == ERANGE) {
        ereport(LogLevel::kError,
                "value \"" + std::string(str) + "\" is out of range for type double precision");
    }
    if (endptr == str) {
        ereport(LogLevel::kError,
                "invalid input syntax for type double precision: \"" + std::string(str) + "\"");
    }
    while (*endptr != '\0' && std::isspace(static_cast<unsigned char>(*endptr))) {
        ++endptr;
    }
    if (*endptr != '\0') {
        ereport(LogLevel::kError,
                "invalid input syntax for type double precision: \"" + std::string(str) + "\"");
    }
    return Float8GetDatum(val);
}

char* float8_out(Datum value) {
    return float8_out_internal(DatumGetFloat8(value));
}

// ---------------------------------------------------------------------------
// text / varchar
// ---------------------------------------------------------------------------

Datum text_in(const char* str) {
    if (str == nullptr) {
        str = "";
    }
    std::size_t len = std::strlen(str);
    // varlena: 4-byte length header (total size) + data
    std::size_t total = sizeof(int32_t) + len;
    char* buf = static_cast<char*>(palloc(total));
    int32_t header = static_cast<int32_t>(total);
    std::memcpy(buf, &header, sizeof(header));
    if (len > 0) {
        std::memcpy(buf + sizeof(int32_t), str, len);
    }
    return TextPGetDatum(buf);
}

char* text_out(Datum value) {
    const char* text = DatumGetTextP(value);
    int data_len = VARSIZE_DATA(text);
    char* result = static_cast<char*>(palloc(static_cast<std::size_t>(data_len) + 1));
    if (data_len > 0) {
        std::memcpy(result, VARDATA(text), static_cast<std::size_t>(data_len));
    }
    result[data_len] = '\0';
    return result;
}

Datum varchar_in(const char* str, int32_t typmod) {
    if (str == nullptr) {
        str = "";
    }
    std::size_t len = std::strlen(str);

    // Apply typmod truncation. typmod encodes VARHDRSZ + max_char_len,
    // so a valid typmod >= VARHDRSZ yields maxlen = typmod - VARHDRSZ.
    if (typmod >= kVarHdrSz) {
        int32_t max_chars = typmod - kVarHdrSz;
        // Count input characters (not bytes) using the database encoding.
        pgcpp::utils::PgEncoding enc = pgcpp::utils::GetDatabaseEncoding();
        int char_len = pgcpp::utils::PgMbstrlenWithLen(enc, str, static_cast<int>(len));
        if (char_len > max_chars) {
            // Determine the byte position of the (max_chars)-th character.
            int mbmaxlen = Mbcharcliplen(str, static_cast<int>(len), max_chars);
            // Per SQL: trailing overflow must be spaces, else error.
            for (int j = mbmaxlen; j < static_cast<int>(len); ++j) {
                if (str[j] != ' ') {
                    char errbuf[128];
                    std::snprintf(errbuf, sizeof(errbuf),
                                  "value too long for type character varying(%d)",
                                  static_cast<int>(max_chars));
                    ereport(LogLevel::kError, errbuf);
                }
            }
            len = static_cast<std::size_t>(mbmaxlen);
        }
    }

    return MakeTextWithLen(str, len);
}

char* varchar_out(Datum value) {
    return text_out(value);
}

Datum varchar_typmod_coerce(Datum source, int32_t typmod, bool is_explicit) {
    // No work if typmod is invalid.
    if (typmod < kVarHdrSz) {
        return source;
    }

    int32_t max_chars = typmod - kVarHdrSz;
    const char* text = DatumGetTextP(source);
    int byte_len = VARSIZE_DATA(text);
    const char* data = VARDATA(text);

    // Count characters in the source value.
    pgcpp::utils::PgEncoding enc = pgcpp::utils::GetDatabaseEncoding();
    int char_len = pgcpp::utils::PgMbstrlenWithLen(enc, data, byte_len);

    // No work if the value already fits.
    if (char_len <= max_chars) {
        return source;
    }

    // Truncate to the first max_chars characters (preserving multi-byte
    // boundaries).
    int mbmaxlen = Mbcharcliplen(data, byte_len, max_chars);

    if (!is_explicit) {
        // Implicit cast: error unless the overflow is all spaces.
        for (int i = mbmaxlen; i < byte_len; ++i) {
            if (data[i] != ' ') {
                char errbuf[128];
                std::snprintf(errbuf, sizeof(errbuf),
                              "value too long for type character varying(%d)",
                              static_cast<int>(max_chars));
                ereport(LogLevel::kError, errbuf);
            }
        }
    }

    return MakeTextWithLen(data, static_cast<std::size_t>(mbmaxlen));
}

// ---------------------------------------------------------------------------
// Comparison functions
// ---------------------------------------------------------------------------

int int2_cmp(Datum a, Datum b) {
    int16_t x = DatumGetInt16(a);
    int16_t y = DatumGetInt16(b);
    if (x < y)
        return -1;
    if (x > y)
        return 1;
    return 0;
}

int int4_cmp(Datum a, Datum b) {
    int32_t x = DatumGetInt32(a);
    int32_t y = DatumGetInt32(b);
    if (x < y)
        return -1;
    if (x > y)
        return 1;
    return 0;
}

int int8_cmp(Datum a, Datum b) {
    int64_t x = DatumGetInt64(a);
    int64_t y = DatumGetInt64(b);
    if (x < y)
        return -1;
    if (x > y)
        return 1;
    return 0;
}

int float8_cmp(Datum a, Datum b) {
    double x = DatumGetFloat8(a);
    double y = DatumGetFloat8(b);
    if (x < y)
        return -1;
    if (x > y)
        return 1;
    return 0;
}

int text_cmp(Datum a, Datum b) {
    const char* ta = DatumGetTextP(a);
    const char* tb = DatumGetTextP(b);
    int la = VARSIZE_DATA(ta);
    int lb = VARSIZE_DATA(tb);
    int min_len = la < lb ? la : lb;
    int cmp = 0;
    if (min_len > 0) {
        cmp = std::memcmp(VARDATA(ta), VARDATA(tb), static_cast<std::size_t>(min_len));
    }
    if (cmp != 0) {
        return cmp < 0 ? -1 : 1;
    }
    if (la < lb)
        return -1;
    if (la > lb)
        return 1;
    return 0;
}

// ---------------------------------------------------------------------------
// Comparison operators (return bool Datum)
// ---------------------------------------------------------------------------

// bool: PostgreSQL orders false < true.
Datum bool_eq(Datum a, Datum b) {
    return BoolGetDatum(DatumGetBool(a) == DatumGetBool(b));
}
Datum bool_ne(Datum a, Datum b) {
    return BoolGetDatum(DatumGetBool(a) != DatumGetBool(b));
}
Datum bool_lt(Datum a, Datum b) {
    return BoolGetDatum(DatumGetBool(a) < DatumGetBool(b));
}
Datum bool_le(Datum a, Datum b) {
    return BoolGetDatum(DatumGetBool(a) <= DatumGetBool(b));
}
Datum bool_gt(Datum a, Datum b) {
    return BoolGetDatum(DatumGetBool(a) > DatumGetBool(b));
}
Datum bool_ge(Datum a, Datum b) {
    return BoolGetDatum(DatumGetBool(a) >= DatumGetBool(b));
}

// int2
Datum int2_eq(Datum a, Datum b) {
    return BoolGetDatum(DatumGetInt16(a) == DatumGetInt16(b));
}
Datum int2_ne(Datum a, Datum b) {
    return BoolGetDatum(DatumGetInt16(a) != DatumGetInt16(b));
}
Datum int2_lt(Datum a, Datum b) {
    return BoolGetDatum(DatumGetInt16(a) < DatumGetInt16(b));
}
Datum int2_le(Datum a, Datum b) {
    return BoolGetDatum(DatumGetInt16(a) <= DatumGetInt16(b));
}
Datum int2_gt(Datum a, Datum b) {
    return BoolGetDatum(DatumGetInt16(a) > DatumGetInt16(b));
}
Datum int2_ge(Datum a, Datum b) {
    return BoolGetDatum(DatumGetInt16(a) >= DatumGetInt16(b));
}

// int4
Datum int4_eq(Datum a, Datum b) {
    return BoolGetDatum(DatumGetInt32(a) == DatumGetInt32(b));
}
Datum int4_ne(Datum a, Datum b) {
    return BoolGetDatum(DatumGetInt32(a) != DatumGetInt32(b));
}
Datum int4_lt(Datum a, Datum b) {
    return BoolGetDatum(DatumGetInt32(a) < DatumGetInt32(b));
}
Datum int4_le(Datum a, Datum b) {
    return BoolGetDatum(DatumGetInt32(a) <= DatumGetInt32(b));
}
Datum int4_gt(Datum a, Datum b) {
    return BoolGetDatum(DatumGetInt32(a) > DatumGetInt32(b));
}
Datum int4_ge(Datum a, Datum b) {
    return BoolGetDatum(DatumGetInt32(a) >= DatumGetInt32(b));
}

// int8
Datum int8_eq(Datum a, Datum b) {
    return BoolGetDatum(DatumGetInt64(a) == DatumGetInt64(b));
}
Datum int8_ne(Datum a, Datum b) {
    return BoolGetDatum(DatumGetInt64(a) != DatumGetInt64(b));
}
Datum int8_lt(Datum a, Datum b) {
    return BoolGetDatum(DatumGetInt64(a) < DatumGetInt64(b));
}
Datum int8_le(Datum a, Datum b) {
    return BoolGetDatum(DatumGetInt64(a) <= DatumGetInt64(b));
}
Datum int8_gt(Datum a, Datum b) {
    return BoolGetDatum(DatumGetInt64(a) > DatumGetInt64(b));
}
Datum int8_ge(Datum a, Datum b) {
    return BoolGetDatum(DatumGetInt64(a) >= DatumGetInt64(b));
}

// float8
Datum float8_eq(Datum a, Datum b) {
    return BoolGetDatum(DatumGetFloat8(a) == DatumGetFloat8(b));
}
Datum float8_ne(Datum a, Datum b) {
    return BoolGetDatum(DatumGetFloat8(a) != DatumGetFloat8(b));
}
Datum float8_lt(Datum a, Datum b) {
    return BoolGetDatum(DatumGetFloat8(a) < DatumGetFloat8(b));
}
Datum float8_le(Datum a, Datum b) {
    return BoolGetDatum(DatumGetFloat8(a) <= DatumGetFloat8(b));
}
Datum float8_gt(Datum a, Datum b) {
    return BoolGetDatum(DatumGetFloat8(a) > DatumGetFloat8(b));
}
Datum float8_ge(Datum a, Datum b) {
    return BoolGetDatum(DatumGetFloat8(a) >= DatumGetFloat8(b));
}

// text
Datum text_eq(Datum a, Datum b) {
    return BoolGetDatum(text_cmp(a, b) == 0);
}
Datum text_ne(Datum a, Datum b) {
    return BoolGetDatum(text_cmp(a, b) != 0);
}
Datum text_lt(Datum a, Datum b) {
    return BoolGetDatum(text_cmp(a, b) < 0);
}
Datum text_le(Datum a, Datum b) {
    return BoolGetDatum(text_cmp(a, b) <= 0);
}
Datum text_gt(Datum a, Datum b) {
    return BoolGetDatum(text_cmp(a, b) > 0);
}
Datum text_ge(Datum a, Datum b) {
    return BoolGetDatum(text_cmp(a, b) >= 0);
}

// ---------------------------------------------------------------------------
// Arithmetic — int4
// ---------------------------------------------------------------------------

Datum int4_pl(Datum a, Datum b) {
    return Int32GetDatum(DatumGetInt32(a) + DatumGetInt32(b));
}

Datum int4_mi(Datum a, Datum b) {
    return Int32GetDatum(DatumGetInt32(a) - DatumGetInt32(b));
}

Datum int4_mul(Datum a, Datum b) {
    return Int32GetDatum(DatumGetInt32(a) * DatumGetInt32(b));
}

Datum int4_div(Datum a, Datum b) {
    int32_t divisor = DatumGetInt32(b);
    if (divisor == 0) {
        ereport(LogLevel::kError, "division by zero");
    }
    return Int32GetDatum(DatumGetInt32(a) / divisor);
}

Datum int4_mod(Datum a, Datum b) {
    int32_t divisor = DatumGetInt32(b);
    if (divisor == 0) {
        ereport(LogLevel::kError, "division by zero");
    }
    return Int32GetDatum(DatumGetInt32(a) % divisor);
}

Datum int4_um(Datum a) {
    return Int32GetDatum(-DatumGetInt32(a));
}

Datum int4_abs(Datum a) {
    int32_t v = DatumGetInt32(a);
    return Int32GetDatum(v < 0 ? -v : v);
}

Datum int4_inc(Datum a) {
    return Int32GetDatum(DatumGetInt32(a) + 1);
}

// ---------------------------------------------------------------------------
// Arithmetic — int2
// ---------------------------------------------------------------------------

Datum int2_pl(Datum a, Datum b) {
    return Int16GetDatum(static_cast<int16_t>(DatumGetInt16(a) + DatumGetInt16(b)));
}

Datum int2_mi(Datum a, Datum b) {
    return Int16GetDatum(static_cast<int16_t>(DatumGetInt16(a) - DatumGetInt16(b)));
}

Datum int2_mul(Datum a, Datum b) {
    return Int16GetDatum(static_cast<int16_t>(DatumGetInt16(a) * DatumGetInt16(b)));
}

Datum int2_div(Datum a, Datum b) {
    int16_t divisor = DatumGetInt16(b);
    if (divisor == 0) {
        ereport(LogLevel::kError, "division by zero");
    }
    return Int16GetDatum(static_cast<int16_t>(DatumGetInt16(a) / divisor));
}

// ---------------------------------------------------------------------------
// Arithmetic — int8
// ---------------------------------------------------------------------------

Datum int8_pl(Datum a, Datum b) {
    return Int64GetDatum(DatumGetInt64(a) + DatumGetInt64(b));
}

Datum int8_mi(Datum a, Datum b) {
    return Int64GetDatum(DatumGetInt64(a) - DatumGetInt64(b));
}

Datum int8_mul(Datum a, Datum b) {
    return Int64GetDatum(DatumGetInt64(a) * DatumGetInt64(b));
}

Datum int8_div(Datum a, Datum b) {
    int64_t divisor = DatumGetInt64(b);
    if (divisor == 0) {
        ereport(LogLevel::kError, "division by zero");
    }
    return Int64GetDatum(DatumGetInt64(a) / divisor);
}

Datum int8_mod(Datum a, Datum b) {
    int64_t divisor = DatumGetInt64(b);
    if (divisor == 0) {
        ereport(LogLevel::kError, "division by zero");
    }
    return Int64GetDatum(DatumGetInt64(a) % divisor);
}

Datum int8_um(Datum a) {
    return Int64GetDatum(-DatumGetInt64(a));
}

Datum int8_abs(Datum a) {
    int64_t v = DatumGetInt64(a);
    return Int64GetDatum(v < 0 ? -v : v);
}

Datum int8_inc(Datum a) {
    return Int64GetDatum(DatumGetInt64(a) + 1);
}

// ---------------------------------------------------------------------------
// Arithmetic — float8
// ---------------------------------------------------------------------------

Datum float8_pl(Datum a, Datum b) {
    return Float8GetDatum(DatumGetFloat8(a) + DatumGetFloat8(b));
}

Datum float8_mi(Datum a, Datum b) {
    return Float8GetDatum(DatumGetFloat8(a) - DatumGetFloat8(b));
}

Datum float8_mul(Datum a, Datum b) {
    return Float8GetDatum(DatumGetFloat8(a) * DatumGetFloat8(b));
}

Datum float8_div(Datum a, Datum b) {
    double divisor = DatumGetFloat8(b);
    if (divisor == 0.0) {
        ereport(LogLevel::kError, "division by zero");
    }
    return Float8GetDatum(DatumGetFloat8(a) / divisor);
}

Datum float8_um(Datum a) {
    return Float8GetDatum(-DatumGetFloat8(a));
}

Datum float8_abs(Datum a) {
    return Float8GetDatum(std::fabs(DatumGetFloat8(a)));
}

Datum float8_ceil(Datum a) {
    return Float8GetDatum(std::ceil(DatumGetFloat8(a)));
}

Datum float8_floor(Datum a) {
    return Float8GetDatum(std::floor(DatumGetFloat8(a)));
}

Datum float8_round(Datum a) {
    return Float8GetDatum(std::round(DatumGetFloat8(a)));
}

Datum float8_trunc(Datum a) {
    return Float8GetDatum(std::trunc(DatumGetFloat8(a)));
}

Datum float8_sign(Datum a) {
    double v = DatumGetFloat8(a);
    if (v > 0) {
        return Float8GetDatum(1.0);
    }
    if (v < 0) {
        return Float8GetDatum(-1.0);
    }
    return Float8GetDatum(0.0);
}

// ---------------------------------------------------------------------------
// Type conversions
// ---------------------------------------------------------------------------

Datum i2toi4(Datum a) {
    return Int32GetDatum(DatumGetInt16(a));
}

Datum i4toi2(Datum a) {
    int32_t v = DatumGetInt32(a);
    if (v < INT16_MIN || v > INT16_MAX) {
        ereport(LogLevel::kError, "smallint out of range");
    }
    return Int16GetDatum(static_cast<int16_t>(v));
}

// ---------------------------------------------------------------------------
// text concatenation
// ---------------------------------------------------------------------------

Datum text_concat(Datum a, Datum b) {
    const char* ta = DatumGetTextP(a);
    const char* tb = DatumGetTextP(b);
    int la = VARSIZE_DATA(ta);
    int lb = VARSIZE_DATA(tb);
    std::size_t total =
        sizeof(int32_t) + static_cast<std::size_t>(la) + static_cast<std::size_t>(lb);
    char* buf = static_cast<char*>(palloc(total));
    int32_t header = static_cast<int32_t>(total);
    std::memcpy(buf, &header, sizeof(header));
    if (la > 0) {
        std::memcpy(buf + sizeof(int32_t), VARDATA(ta), static_cast<std::size_t>(la));
    }
    if (lb > 0) {
        std::memcpy(buf + sizeof(int32_t) + static_cast<std::size_t>(la), VARDATA(tb),
                    static_cast<std::size_t>(lb));
    }
    return TextPGetDatum(buf);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

Datum MakeTextDatum(std::string_view str) {
    std::size_t total = sizeof(int32_t) + str.size();
    char* buf = static_cast<char*>(palloc(total));
    int32_t header = static_cast<int32_t>(total);
    std::memcpy(buf, &header, sizeof(header));
    if (!str.empty()) {
        std::memcpy(buf + sizeof(int32_t), str.data(), str.size());
    }
    return TextPGetDatum(buf);
}

std::string TextDatumToString(Datum datum) {
    const char* text = DatumGetTextP(datum);
    int data_len = VARSIZE_DATA(text);
    if (data_len <= 0) {
        return std::string();
    }
    return std::string(VARDATA(text), static_cast<std::size_t>(data_len));
}

}  // namespace pgcpp::types

#include "mytoydb/types/builtins.h"

#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "mytoydb/common/error/elog.h"
#include "mytoydb/common/memory/memory_context.h"

namespace mytoydb::types {

using mytoydb::error::LogLevel;
using mytoydb::memory::palloc;

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
    ereport(LogLevel::kError, "invalid input syntax for type boolean: \"" + std::string(s) + "\"");
}

char* bool_out(Datum value) {
    return PallocCString(DatumGetBool(value) ? "t" : "f");
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
    double val = DatumGetFloat8(value);
    // PostgreSQL uses "%.15g" for shortest round-trip representation.
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%.15g", val);
    return PallocCString(buf);
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

Datum varchar_in(const char* str) {
    return text_in(str);
}

char* varchar_out(Datum value) {
    return text_out(value);
}

// ---------------------------------------------------------------------------
// Comparison functions
// ---------------------------------------------------------------------------

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

}  // namespace mytoydb::types

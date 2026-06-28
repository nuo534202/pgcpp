// misc_types.cpp — implementations for char, name, xid, xid8, tid, pg_lsn, bytea.
//
// Mirrors PostgreSQL's utils/adt/char.c, name.c, xid.c, tid.c, pg_lsn.c, and
// the bytea in/out routines in utils/adt/varlena.c.

#include "mytoydb/types/misc_types.hpp"

#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "mytoydb/common/error/elog.hpp"
#include "mytoydb/common/memory/memory_context.hpp"

namespace mytoydb::types {

using mytoydb::error::LogLevel;
using mytoydb::memory::palloc;

namespace {

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
// "char"
// ---------------------------------------------------------------------------

Datum char_in(const char* str) {
    if (str == nullptr || str[0] == '\0') {
        ereport(LogLevel::kError, "invalid input syntax for type \"char\": NULL");
    }
    return Datum(static_cast<uint8_t>(str[0]));
}

char* char_out(Datum value) {
    char buf[2] = {static_cast<char>(static_cast<uint8_t>(value)), '\0'};
    return PallocCString(buf);
}

int char_cmp(Datum a, Datum b) {
    int8_t x = static_cast<int8_t>(static_cast<uint8_t>(a));
    int8_t y = static_cast<int8_t>(static_cast<uint8_t>(b));
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

Datum char_eq(Datum a, Datum b) {
    return BoolGetDatum(char_cmp(a, b) == 0);
}
Datum char_lt(Datum a, Datum b) {
    return BoolGetDatum(char_cmp(a, b) < 0);
}

// ---------------------------------------------------------------------------
// name
// ---------------------------------------------------------------------------

Datum name_in(const char* str) {
    if (str == nullptr) {
        str = "";
    }
    return MakeNameDatum(str);
}

char* name_out(Datum value) {
    return PallocCString(NameDatumToCString(value));
}

int name_cmp(Datum a, Datum b) {
    const char* sa = NameDatumToCString(a);
    const char* sb = NameDatumToCString(b);
    int cmp = std::strncmp(sa, sb, kNameDataLen - 1);
    return (cmp < 0) ? -1 : (cmp > 0) ? 1 : 0;
}

Datum name_eq(Datum a, Datum b) {
    return BoolGetDatum(name_cmp(a, b) == 0);
}
Datum name_ne(Datum a, Datum b) {
    return BoolGetDatum(name_cmp(a, b) != 0);
}
Datum name_lt(Datum a, Datum b) {
    return BoolGetDatum(name_cmp(a, b) < 0);
}
Datum name_le(Datum a, Datum b) {
    return BoolGetDatum(name_cmp(a, b) <= 0);
}
Datum name_gt(Datum a, Datum b) {
    return BoolGetDatum(name_cmp(a, b) > 0);
}
Datum name_ge(Datum a, Datum b) {
    return BoolGetDatum(name_cmp(a, b) >= 0);
}

Datum MakeNameDatum(const char* str) {
    if (str == nullptr) {
        str = "";
    }
    auto* name = static_cast<NameData*>(palloc(sizeof(NameData)));
    std::memset(name->data, 0, kNameDataLen);
    std::strncpy(name->data, str, kNameDataLen - 1);
    name->data[kNameDataLen - 1] = '\0';
    return reinterpret_cast<Datum>(name);
}

const char* NameDatumToCString(Datum value) {
    return reinterpret_cast<NameData*>(value)->data;
}

// ---------------------------------------------------------------------------
// xid / xid8
// ---------------------------------------------------------------------------

Datum xid_in(const char* str) {
    if (str == nullptr) {
        ereport(LogLevel::kError, "invalid input syntax for type xid: NULL");
    }
    errno = 0;
    char* endptr = nullptr;
    unsigned long val = std::strtoul(str, &endptr, 10);
    if (errno == ERANGE || val > UINT32_MAX) {
        ereport(LogLevel::kError, "value out of range for type xid: \"" + std::string(str) + "\"");
    }
    if (endptr == str) {
        ereport(LogLevel::kError,
                "invalid input syntax for type xid: \"" + std::string(str) + "\"");
    }
    while (*endptr != '\0' && std::isspace(static_cast<unsigned char>(*endptr))) {
        ++endptr;
    }
    if (*endptr != '\0') {
        ereport(LogLevel::kError,
                "invalid input syntax for type xid: \"" + std::string(str) + "\"");
    }
    return Datum(static_cast<uint32_t>(val));
}

char* xid_out(Datum value) {
    return PallocCString(std::to_string(static_cast<uint32_t>(value)));
}

Datum xid8_in(const char* str) {
    if (str == nullptr) {
        ereport(LogLevel::kError, "invalid input syntax for type xid8: NULL");
    }
    errno = 0;
    char* endptr = nullptr;
    unsigned long long val = std::strtoull(str, &endptr, 10);
    if (errno == ERANGE) {
        ereport(LogLevel::kError, "value out of range for type xid8: \"" + std::string(str) + "\"");
    }
    if (endptr == str) {
        ereport(LogLevel::kError,
                "invalid input syntax for type xid8: \"" + std::string(str) + "\"");
    }
    while (*endptr != '\0' && std::isspace(static_cast<unsigned char>(*endptr))) {
        ++endptr;
    }
    if (*endptr != '\0') {
        ereport(LogLevel::kError,
                "invalid input syntax for type xid8: \"" + std::string(str) + "\"");
    }
    return Datum(static_cast<uint64_t>(val));
}

char* xid8_out(Datum value) {
    return PallocCString(std::to_string(static_cast<uint64_t>(value)));
}

int xid_cmp(Datum a, Datum b) {
    uint32_t x = static_cast<uint32_t>(a);
    uint32_t y = static_cast<uint32_t>(b);
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

Datum xid_eq(Datum a, Datum b) {
    return BoolGetDatum(xid_cmp(a, b) == 0);
}

// ---------------------------------------------------------------------------
// tid
// ---------------------------------------------------------------------------

Datum tid_in(const char* str) {
    if (str == nullptr) {
        ereport(LogLevel::kError, "invalid input syntax for type tid: NULL");
    }
    while (*str != '\0' && std::isspace(static_cast<unsigned char>(*str))) {
        ++str;
    }
    if (*str != '(') {
        ereport(LogLevel::kError,
                "invalid input syntax for type tid: \"" + std::string(str) + "\"");
    }
    ++str;
    errno = 0;
    char* endptr = nullptr;
    unsigned long block = std::strtoul(str, &endptr, 10);
    if (errno == ERANGE || endptr == str) {
        ereport(LogLevel::kError,
                "invalid input syntax for type tid: \"" + std::string(str) + "\"");
    }
    while (*endptr != '\0' && std::isspace(static_cast<unsigned char>(*endptr))) {
        ++endptr;
    }
    if (*endptr != ',') {
        ereport(LogLevel::kError, "invalid input syntax for type tid: expected ','");
    }
    ++endptr;
    char* endptr2 = nullptr;
    unsigned long offset = std::strtoul(endptr, &endptr2, 10);
    if (errno == ERANGE || endptr2 == endptr) {
        ereport(LogLevel::kError, "invalid input syntax for type tid");
    }
    while (*endptr2 != '\0' && std::isspace(static_cast<unsigned char>(*endptr2))) {
        ++endptr2;
    }
    if (*endptr2 != ')') {
        ereport(LogLevel::kError, "invalid input syntax for type tid: expected ')'");
    }
    auto* tid = static_cast<ItemPointerData*>(palloc(sizeof(ItemPointerData)));
    tid->block_num = static_cast<uint32_t>(block);
    tid->offset_num = static_cast<uint16_t>(offset);
    return reinterpret_cast<Datum>(tid);
}

char* tid_out(Datum value) {
    const auto* tid = reinterpret_cast<ItemPointerData*>(value);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "(%u,%u)", tid->block_num, tid->offset_num);
    return PallocCString(buf);
}

int tid_cmp(Datum a, Datum b) {
    const auto* ta = reinterpret_cast<ItemPointerData*>(a);
    const auto* tb = reinterpret_cast<ItemPointerData*>(b);
    if (ta->block_num != tb->block_num) {
        return (ta->block_num < tb->block_num) ? -1 : 1;
    }
    if (ta->offset_num != tb->offset_num) {
        return (ta->offset_num < tb->offset_num) ? -1 : 1;
    }
    return 0;
}

Datum tid_eq(Datum a, Datum b) {
    return BoolGetDatum(tid_cmp(a, b) == 0);
}
Datum tid_lt(Datum a, Datum b) {
    return BoolGetDatum(tid_cmp(a, b) < 0);
}
Datum tid_gt(Datum a, Datum b) {
    return BoolGetDatum(tid_cmp(a, b) > 0);
}

// ---------------------------------------------------------------------------
// pg_lsn
// ---------------------------------------------------------------------------

Datum pg_lsn_in(const char* str) {
    if (str == nullptr) {
        ereport(LogLevel::kError, "invalid input syntax for type pg_lsn: NULL");
    }
    unsigned int hi = 0;
    unsigned int lo = 0;
    int consumed = std::sscanf(str, "%x/%x", &hi, &lo);
    if (consumed != 2) {
        ereport(LogLevel::kError,
                "invalid input syntax for type pg_lsn: \"" + std::string(str) + "\"");
    }
    uint64_t v = (static_cast<uint64_t>(hi) << 32) | lo;
    return Datum(v);
}

char* pg_lsn_out(Datum value) {
    uint64_t v = static_cast<uint64_t>(value);
    uint32_t hi = static_cast<uint32_t>(v >> 32);
    uint32_t lo = static_cast<uint32_t>(v);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%X/%X", hi, lo);
    return PallocCString(buf);
}

int pg_lsn_cmp(Datum a, Datum b) {
    uint64_t x = static_cast<uint64_t>(a);
    uint64_t y = static_cast<uint64_t>(b);
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

Datum pg_lsn_eq(Datum a, Datum b) {
    return BoolGetDatum(pg_lsn_cmp(a, b) == 0);
}
Datum pg_lsn_lt(Datum a, Datum b) {
    return BoolGetDatum(pg_lsn_cmp(a, b) < 0);
}

Datum pg_lsn_add(Datum a, Datum b) {
    uint64_t x = static_cast<uint64_t>(a);
    int64_t delta = static_cast<int64_t>(b);
    return Datum(static_cast<uint64_t>(static_cast<int64_t>(x) + delta));
}

// ---------------------------------------------------------------------------
// bytea — escape format with \xxx octal and \\ backslash escapes.
// ---------------------------------------------------------------------------

namespace {

// Decode the PostgreSQL bytea "escape" format into a fresh std::string.
std::string ByteaDecode(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        if (c == '\\') {
            // Either "\\" or "\xxx" (1-3 octal digits).
            if (i + 1 < input.size() && input[i + 1] == '\\') {
                out.push_back('\\');
                ++i;
                continue;
            }
            // Try to parse up to three octal digits.
            int val = 0;
            int count = 0;
            while (count < 3 && i + 1 + count < input.size() &&
                   std::isdigit(static_cast<unsigned char>(input[i + 1 + count])) &&
                   input[i + 1 + count] >= '0' && input[i + 1 + count] <= '7') {
                val = val * 8 + (input[i + 1 + count] - '0');
                ++count;
            }
            if (count == 0) {
                // Stray backslash — keep it.
                out.push_back('\\');
            } else {
                out.push_back(static_cast<char>(val));
                i += count;
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// Encode a byte sequence into PostgreSQL bytea escape format.
std::string ByteaEncode(const char* data, std::size_t len) {
    std::string out;
    out.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        unsigned char b = static_cast<unsigned char>(data[i]);
        if (b == '\\') {
            out.append("\\\\");
        } else if (b < 0x20 || b >= 0x7f) {
            char buf[5];
            std::snprintf(buf, sizeof(buf), "\\%03o", b);
            out.append(buf);
        } else {
            out.push_back(static_cast<char>(b));
        }
    }
    return out;
}

}  // namespace

Datum bytea_in(const char* str) {
    if (str == nullptr) {
        str = "";
    }
    std::string decoded = ByteaDecode(str);
    std::size_t total = sizeof(int32_t) + decoded.size();
    char* buf = static_cast<char*>(palloc(total));
    int32_t header = static_cast<int32_t>(total);
    std::memcpy(buf, &header, sizeof(header));
    if (!decoded.empty()) {
        std::memcpy(buf + sizeof(int32_t), decoded.data(), decoded.size());
    }
    return reinterpret_cast<Datum>(buf);
}

char* bytea_out(Datum value) {
    const char* text = DatumGetTextP(value);
    int data_len = VARSIZE_DATA(text);
    std::string encoded = ByteaEncode(VARDATA(text), static_cast<std::size_t>(data_len));
    return PallocCString(encoded);
}

int bytea_cmp(Datum a, Datum b) {
    const char* ta = DatumGetTextP(a);
    const char* tb = DatumGetTextP(b);
    int la = VARSIZE_DATA(ta);
    int lb = VARSIZE_DATA(tb);
    int min_len = la < lb ? la : lb;
    int cmp = std::memcmp(VARDATA(ta), VARDATA(tb), static_cast<std::size_t>(min_len));
    if (cmp != 0) {
        return (cmp < 0) ? -1 : 1;
    }
    return (la < lb) ? -1 : (la > lb) ? 1 : 0;
}

Datum bytea_eq(Datum a, Datum b) {
    return BoolGetDatum(bytea_cmp(a, b) == 0);
}

Datum bytea_concat(Datum a, Datum b) {
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
        std::memcpy(buf + sizeof(int32_t) + la, VARDATA(tb), static_cast<std::size_t>(lb));
    }
    return reinterpret_cast<Datum>(buf);
}

Datum bytea_length(Datum value) {
    const char* text = DatumGetTextP(value);
    return Int32GetDatum(VARSIZE_DATA(text));
}

}  // namespace mytoydb::types

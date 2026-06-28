// uuid.cpp — UUID type implementation (PostgreSQL utils/adt/uuid.c).

#include "mytoydb/types/uuid.hpp"

#include <cctype>
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

bool IsHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int HexVal(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return c - 'A' + 10;
}

}  // namespace

Datum MakeUuidDatum(const UuidData& uuid) {
    auto* p = static_cast<UuidData*>(palloc(sizeof(UuidData)));
    std::memcpy(p->bytes, uuid.bytes, 16);
    return reinterpret_cast<Datum>(p);
}

Datum uuid_in(const char* str) {
    if (str == nullptr) {
        ereport(LogLevel::kError, "invalid input syntax for type uuid: NULL");
    }
    UuidData uuid{};
    std::size_t hex_seen = 0;
    for (std::size_t i = 0; str[i] != '\0' && hex_seen < 32; ++i) {
        char c = str[i];
        if (c == '-') {
            continue;
        }
        if (c == '{' && i == 0) {
            // Allow brace-wrapped form.
            continue;
        }
        if (c == '}' && str[i + 1] == '\0') {
            continue;
        }
        if (!IsHex(c)) {
            ereport(LogLevel::kError,
                    "invalid input syntax for type uuid: \"" + std::string(str) + "\"");
        }
        int high = HexVal(c);
        ++i;
        if (str[i] == '\0' || !IsHex(str[i])) {
            ereport(LogLevel::kError,
                    "invalid input syntax for type uuid: \"" + std::string(str) + "\"");
        }
        int low = HexVal(str[i]);
        if (hex_seen >= 16) {
            ereport(LogLevel::kError, "uuid input too long: \"" + std::string(str) + "\"");
        }
        uuid.bytes[hex_seen] = static_cast<uint8_t>((high << 4) | low);
        ++hex_seen;
    }
    if (hex_seen != 16) {
        ereport(LogLevel::kError,
                "invalid input syntax for type uuid: \"" + std::string(str) + "\"");
    }
    return MakeUuidDatum(uuid);
}

char* uuid_out(Datum value) {
    const auto* u = DatumGetUuid(value);
    char buf[40];
    std::snprintf(buf, sizeof(buf),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  u->bytes[0], u->bytes[1], u->bytes[2], u->bytes[3], u->bytes[4], u->bytes[5],
                  u->bytes[6], u->bytes[7], u->bytes[8], u->bytes[9], u->bytes[10], u->bytes[11],
                  u->bytes[12], u->bytes[13], u->bytes[14], u->bytes[15]);
    return PallocCString(buf);
}

int uuid_cmp(Datum a, Datum b) {
    const auto* x = DatumGetUuid(a);
    const auto* y = DatumGetUuid(b);
    int cmp = std::memcmp(x->bytes, y->bytes, 16);
    return (cmp < 0) ? -1 : (cmp > 0) ? 1 : 0;
}

Datum uuid_eq(Datum a, Datum b) {
    return BoolGetDatum(uuid_cmp(a, b) == 0);
}
Datum uuid_ne(Datum a, Datum b) {
    return BoolGetDatum(uuid_cmp(a, b) != 0);
}
Datum uuid_lt(Datum a, Datum b) {
    return BoolGetDatum(uuid_cmp(a, b) < 0);
}
Datum uuid_le(Datum a, Datum b) {
    return BoolGetDatum(uuid_cmp(a, b) <= 0);
}
Datum uuid_gt(Datum a, Datum b) {
    return BoolGetDatum(uuid_cmp(a, b) > 0);
}
Datum uuid_ge(Datum a, Datum b) {
    return BoolGetDatum(uuid_cmp(a, b) >= 0);
}

}  // namespace mytoydb::types

#pragma once

#include <cstdint>
#include <cstring>

namespace mytoydb::types {

// Datum — the fundamental PostgreSQL value type.
// On 64-bit systems, it is 8 bytes and can hold any by-value type up to 8 bytes.
using Datum = uintptr_t;

// Type OIDs (PostgreSQL constants, kept as constexpr)
constexpr uint32_t kInvalidOid = 0;
constexpr uint32_t kBoolOid = 16;
constexpr uint32_t kInt8Oid = 20;  // int64
constexpr uint32_t kInt2Oid = 21;  // int16
constexpr uint32_t kInt4Oid = 23;  // int32
constexpr uint32_t kTextOid = 25;
constexpr uint32_t kFloat4Oid = 700;
constexpr uint32_t kFloat8Oid = 708;
constexpr uint32_t kVarcharOid = 1043;
constexpr uint32_t kDateOid = 1082;
constexpr uint32_t kTimestampOid = 1114;
constexpr uint32_t kTimestamptzOid = 1184;

// DatumGet* / *GetDatum conversions (inline constexpr / inline)
inline bool DatumGetBool(Datum x) {
    return x != 0;
}
inline Datum BoolGetDatum(bool x) {
    return Datum(x ? 1 : 0);
}

inline int8_t DatumGetInt8(Datum x) {
    return static_cast<int8_t>(x);
}
inline Datum Int8GetDatum(int8_t x) {
    return Datum(x);
}

inline int16_t DatumGetInt16(Datum x) {
    return static_cast<int16_t>(x);
}
inline Datum Int16GetDatum(int16_t x) {
    return Datum(static_cast<uint16_t>(x));
}

inline int32_t DatumGetInt32(Datum x) {
    return static_cast<int32_t>(x);
}
inline Datum Int32GetDatum(int32_t x) {
    return Datum(static_cast<uint32_t>(x));
}

inline int64_t DatumGetInt64(Datum x) {
    return static_cast<int64_t>(x);
}
inline Datum Int64GetDatum(int64_t x) {
    return Datum(static_cast<uint64_t>(x));
}

inline float DatumGetFloat4(Datum x) {
    float f;
    std::memcpy(&f, &x, sizeof(f));
    return f;
}
inline Datum Float4GetDatum(float x) {
    Datum d;
    std::memcpy(&d, &x, sizeof(x));
    return d;
}

inline double DatumGetFloat8(Datum x) {
    double d;
    std::memcpy(&d, &x, sizeof(d));
    return d;
}
inline Datum Float8GetDatum(double x) {
    Datum d;
    std::memcpy(&d, &x, sizeof(x));
    return d;
}

// For by-reference types (text, varchar), Datum is a pointer.
// The pointed-to data has a 4-byte length prefix (varlena) followed by data.
// This matches PostgreSQL's text/varchar storage format.

// varlena header: first 4 bytes are the total length (including header)
struct Varlena {
    int32_t vl_len;
    char vl_dat[1];  // flexible array member (C-style, kept for compatibility)
};

inline char* DatumGetTextP(Datum x) {
    return reinterpret_cast<char*>(x);
}
inline Datum TextPGetDatum(void* x) {
    return Datum(x);
}

// Helper: get the data pointer and length from a text Datum.
inline const char* VARDATA(const char* text) {
    return text + sizeof(int32_t);
}
inline int VARSIZE(const char* text) {
    int32_t len;
    std::memcpy(&len, text, sizeof(len));
    return len;
}
inline int VARSIZE_DATA(const char* text) {
    return VARSIZE(text) - sizeof(int32_t);
}

}  // namespace mytoydb::types

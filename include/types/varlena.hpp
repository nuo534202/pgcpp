// varlena.hpp — Variable-length attribute (varlena) header structures.
//
// Converted from PostgreSQL 15's src/include/varatt.h and c.h.
//
// A varlena value is stored inline in a heap tuple with a length header.
// pgcpp uses a simplified 4-byte header format (PostgreSQL also supports
// 1-byte short headers, but pgcpp always uses 4 bytes for simplicity).
//
// Three storage modes are distinguished by the two high bits of the header:
//
//   Normal (bits 30-31 = 00):
//     [int32 total_size] [data...]
//     total_size includes the 4-byte header.
//
//   Compressed inline (bit 30 = 1, bit 31 = 0):
//     [int32 total_size | kVarAttCompressed] [int32 raw_size] [compressed data...]
//     total_size (with bit 30 cleared) includes header + raw_size + compressed data.
//     raw_size is the original uncompressed data size (excluding headers).
//
//   External TOAST pointer (bit 31 = 1):
//     [int32 kVarAttExternalMarker] [int32 raw_size] [Oid value_id] [Oid toast_relid]
//     The full 16-byte struct (varatt_external) replaces the varlena inline.
//     raw_size > 0 means the external value is also compressed.
#pragma once

#include <cstdint>
#include <cstring>

#include "catalog/catalog.hpp"

namespace pgcpp::types {

// --- varlena header flags ---
//
// Bit 31 (0x80000000): external TOAST pointer — the inline "varlena" is
//   actually a 16-byte varatt_external struct.
// Bit 30 (0x40000000): compressed inline — the varlena data is compressed
//   and preceded by a 4-byte raw (uncompressed) size.
// Bits 0-29: total inline size (including the 4-byte header) for normal
//   and compressed-inline modes.
constexpr uint32_t kVarAttExternal = 0x80000000u;
constexpr uint32_t kVarAttCompressed = 0x40000000u;
constexpr uint32_t kVarAttSizeMask = 0x3FFFFFFFu;  // bits 0-29

// varatt_external — the inline pointer for an out-of-line TOAST value.
//
// This replaces the varlena data in the heap tuple when a value is stored
// in the TOAST table. The struct is exactly 16 bytes.
struct varatt_external {
    uint32_t va_header = kVarAttExternal;  // marker (bit 31 set)
    int32_t va_rawsize = 0;                // original data size (0 = not compressed externally)
    pgcpp::catalog::Oid va_valueid = pgcpp::catalog::kInvalidOid;     // TOAST chunk ID
    pgcpp::catalog::Oid va_toastrelid = pgcpp::catalog::kInvalidOid;  // TOAST table OID
};
static_assert(sizeof(varatt_external) == 16, "varatt_external must be 16 bytes");

// --- varlena inspection macros ---

// Read the 4-byte varlena header as uint32_t.
inline uint32_t VARATT_HEADER(const char* text) {
    uint32_t hdr;
    std::memcpy(&hdr, text, sizeof(hdr));
    return hdr;
}

// VARATT_IS_EXTERNAL — true if the varlena is a TOAST external pointer.
inline bool VARATT_IS_EXTERNAL(const char* text) {
    return (VARATT_HEADER(text) & kVarAttExternal) != 0;
}

// VARATT_IS_COMPRESSED — true if the varlena is compressed inline.
inline bool VARATT_IS_COMPRESSED(const char* text) {
    uint32_t hdr = VARATT_HEADER(text);
    return (hdr & kVarAttExternal) == 0 && (hdr & kVarAttCompressed) != 0;
}

// VARATT_IS_4B — true for a normal 4-byte-header varlena (not external/compressed).
inline bool VARATT_IS_4B(const char* text) {
    return (VARATT_HEADER(text) & (kVarAttExternal | kVarAttCompressed)) == 0;
}

// VARSIZE_ANY — total inline size of any varlena value (including header).
// For external pointers, this is sizeof(varatt_external) = 16.
// For compressed inline, this is the header with bit 30 cleared.
// For normal, this is the raw header value.
inline uint32_t VARSIZE_ANY(const char* text) {
    uint32_t hdr = VARATT_HEADER(text);
    if ((hdr & kVarAttExternal) != 0) {
        return sizeof(varatt_external);
    }
    return hdr & kVarAttSizeMask;
}

// VARDATA_4B — pointer to the data portion of a normal 4-byte varlena.
inline char* VARDATA_4B(char* text) {
    return text + sizeof(uint32_t);
}
inline const char* VARDATA_4B(const char* text) {
    return text + sizeof(uint32_t);
}

// VARDATA_COMPRESSED — pointer past the raw_size field of a compressed varlena.
inline char* VARDATA_COMPRESSED(char* text) {
    return text + sizeof(uint32_t) + sizeof(int32_t);  // header + rawsize
}
inline const char* VARDATA_COMPRESSED(const char* text) {
    return text + sizeof(uint32_t) + sizeof(int32_t);
}

// VARSIZE_COMPRESSED_DATA — size of the compressed data payload.
inline uint32_t VARSIZE_COMPRESSED_DATA(const char* text) {
    uint32_t total = VARSIZE_ANY(text);
    return total - static_cast<uint32_t>(sizeof(uint32_t)) - static_cast<uint32_t>(sizeof(int32_t));
}

// VARATT_RAW_SIZE — for compressed inline, the original uncompressed data size.
inline int32_t VARATT_RAW_SIZE(const char* text) {
    int32_t rawsize;
    std::memcpy(&rawsize, text + sizeof(uint32_t), sizeof(rawsize));
    return rawsize;
}

// SET_VARSIZE_4B — write a normal 4-byte varlena header.
inline void SET_VARSIZE_4B(char* text, uint32_t total_size) {
    uint32_t hdr = total_size & kVarAttSizeMask;
    std::memcpy(text, &hdr, sizeof(hdr));
}

// SET_VARSIZE_COMPRESSED — write a compressed-inline varlena header.
inline void SET_VARSIZE_COMPRESSED(char* text, uint32_t total_size) {
    uint32_t hdr = (total_size & kVarAttSizeMask) | kVarAttCompressed;
    std::memcpy(text, &hdr, sizeof(hdr));
}

// SET_VARSIZE_EXTERNAL — write an external TOAST pointer header.
inline void SET_VARSIZE_EXTERNAL(char* text) {
    uint32_t hdr = kVarAttExternal;
    std::memcpy(text, &hdr, sizeof(hdr));
}

// VARDATA_EXTERNAL — get a pointer to the varatt_external struct.
inline varatt_external* VARDATA_EXTERNAL(char* text) {
    return reinterpret_cast<varatt_external*>(text);
}
inline const varatt_external* VARDATA_EXTERNAL(const char* text) {
    return reinterpret_cast<const varatt_external*>(text);
}

}  // namespace pgcpp::types

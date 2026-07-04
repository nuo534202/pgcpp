// crc32c.cpp — CRC32C (Castagnoli) implementation.
//
// Provides two implementations selected at runtime:
//   1. Software: Sarwate table-driven algorithm (256-entry lookup table).
//   2. SSE4.2: hardware-accelerated via __builtin_ia32_crc32 intrinsics.
//
// Both produce identical results for the same input. The Castagnoli polynomial
// is 0x1EDC6F41 (reflected: 0x82F63B78).
//
// Convention (matches PostgreSQL):
//   INIT  : crc = 0xFFFFFFFF
//   UPDATE: crc = table[(crc ^ byte) & 0xFF] ^ (crc >> 8)
//   FIN   : crc ^= 0xFFFFFFFF
//
// Known test vector: CRC32C("123456789") = 0xE3069283
#include "transaction/crc32c.hpp"

#include <array>
#include <cstring>

namespace pgcpp::transaction {

namespace {

// Reflected Castagnoli polynomial for the software implementation.
constexpr uint32_t kCastagnoliPolyReflected = 0x82F63B78;

// 256-entry lookup table for the Sarwate algorithm.
struct Crc32CTable {
    std::array<uint32_t, 256> entries{};

    constexpr Crc32CTable() : entries{} {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t crc = i;
            for (int j = 0; j < 8; j++) {
                if (crc & 1) {
                    crc = (crc >> 1) ^ kCastagnoliPolyReflected;
                } else {
                    crc >>= 1;
                }
            }
            entries[i] = crc;
        }
    }
};

constexpr Crc32CTable kCrc32CTable;

// Software fallback: process one byte at a time using the lookup table.
uint32_t Crc32CUpdateSoftware(uint32_t crc, const uint8_t* data,
                              std::size_t len) {
    for (std::size_t i = 0; i < len; i++) {
        crc = kCrc32CTable.entries[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

#if defined(__x86_64__)
// SSE4.2 hardware-accelerated update.
// Uses __builtin_ia32_crc32di for 8-byte chunks, __builtin_ia32_crc32si for
// 4-byte chunks, and __builtin_ia32_crc32qi for trailing bytes.
// The target("sse4.2") attribute enables SSE4.2 codegen for this function
// only, so the rest of the file compiles without -msse4.2.
__attribute__((target("sse4.2")))
uint32_t Crc32CUpdateSse42(uint32_t crc, const uint8_t* data,
                           std::size_t len) {
    // Process 8-byte chunks.
    std::size_t i = 0;
    while (i + 8 <= len) {
        uint64_t chunk;
        std::memcpy(&chunk, data + i, 8);
        crc = static_cast<uint32_t>(
            __builtin_ia32_crc32di(static_cast<uint64_t>(crc), chunk));
        i += 8;
    }
    // Process 4-byte chunk if present.
    if (i + 4 <= len) {
        uint32_t chunk;
        std::memcpy(&chunk, data + i, 4);
        crc = __builtin_ia32_crc32si(crc, chunk);
        i += 4;
    }
    // Process remaining bytes one at a time.
    while (i < len) {
        crc = __builtin_ia32_crc32qi(crc, data[i]);
        i++;
    }
    return crc;
}

// Runtime detection: returns true if SSE4.2 is available.
bool HasSse42() {
    return __builtin_cpu_supports("sse4.2");
}
#endif  // __x86_64__

}  // namespace

Crc32C::Crc32C() : crc_(0xFFFFFFFFu) {}

void Crc32C::Reset() {
    crc_ = 0xFFFFFFFFu;
}

void Crc32C::Update(const void* data, std::size_t len) {
    if (data == nullptr || len == 0) {
        return;
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
#if defined(__x86_64__)
    static const bool kHasSse42 = HasSse42();
    if (kHasSse42) {
        crc_ = Crc32CUpdateSse42(crc_, bytes, len);
        return;
    }
#endif
    crc_ = Crc32CUpdateSoftware(crc_, bytes, len);
}

uint32_t Crc32C::Finalize() {
    crc_ ^= 0xFFFFFFFFu;
    return crc_;
}

uint32_t Crc32CCompute(const void* data, std::size_t len) {
    Crc32C crc;
    crc.Update(data, len);
    return crc.Finalize();
}

}  // namespace pgcpp::transaction

// crc32c.h — CRC32C (Castagnoli) computation for WAL record verification.
//
// PostgreSQL uses CRC32C (polynomial 0x1EDC6F41) to verify WAL record
// integrity. pgcpp provides two implementations:
//   1. Software fallback: Sarwate algorithm with a 256-entry lookup table.
//   2. SSE4.2 hardware acceleration via __builtin_ia32_crc32 intrinsics.
//
// The implementation is selected at runtime via __builtin_cpu_supports("sse4.2").
// Both produce identical results for the same input.
//
// Usage:
//   pgcpp::transaction::Crc32C crc;
//   crc.Update(data1, len1);
//   crc.Update(data2, len2);
//   uint32_t result = crc.Finalize();
//
// Or the one-shot helper:
//   uint32_t crc = Crc32C::Compute(data, len);
#pragma once

#include <cstddef>
#include <cstdint>

namespace pgcpp::transaction {

// Crc32C — incremental CRC32C calculator.
//
// Uses the Castagnoli polynomial (used by SSE4.2's CRC32 instruction).
// The incremental API mirrors PostgreSQL's INIT_CRC32C / COMP_CRC32C /
// FIN_CRC32C macros.
class Crc32C {
public:
    // Create a new CRC accumulator initialized to the starting value.
    Crc32C();

    // Reset the accumulator to the starting value (equivalent to INIT_CRC32C).
    void Reset();

    // Feed `len` bytes into the accumulator (equivalent to COMP_CRC32C).
    void Update(const void* data, std::size_t len);

    // Finalize and return the CRC value (equivalent to FIN_CRC32C).
    // After this call, the accumulator holds the finalized value; Reset()
    // must be called before reusing for a new computation.
    uint32_t Finalize();

private:
    uint32_t crc_;
};

// Crc32CCompute — one-shot convenience: compute CRC32C of a single buffer.
uint32_t Crc32CCompute(const void* data, std::size_t len);

}  // namespace pgcpp::transaction

// hyperloglog.hpp — HyperLogLog cardinality estimator.
//
// Mirrors PostgreSQL's src/backend/lib/hyperloglog.c. A standard HyperLogLog
// probabilistic counter that estimates the number of distinct values seen
// via Add() calls. Uses 2^p 6-bit registers (PostgreSQL default p=14,
// yielding 16384 registers and ~0.81% relative error).
//
// Algorithm (Flajolet et al. 2007):
//   1. Hash each input to a 64-bit value.
//   2. Use the high p bits as register index j in [0, 2^p).
//   3. Count leading zeros + 1 in the remaining (64 - p) bits → rho.
//   4. registers_[j] = max(registers_[j], rho).
//   5. On Estimate():
//        E = alpha * m^2 / sum(2^(-registers_[j]))
//      with bias corrections:
//        - small range (E <= 2.5*m): use LinearCounting on zero registers.
//        - large range (E > 2^32/30): apply multiplicative correction.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mytoydb::lib {

class HyperLogLog {
public:
    // Constructs a HyperLogLog with 2^register_bits registers.
    // register_bits must be in [4, 17]; defaults to 14 (PG default).
    explicit HyperLogLog(int register_bits = kDefaultRegisterBits);

    // PG: InitHyperLogLog — reinitialize the estimator to an empty state.
    void Reset();

    // PG: AddToHyperLogLog — incorporate a hashed element.
    // Note: PostgreSQL uses a uint64 hash, so callers should hash first.
    void AddHashed(uint64_t hash);

    // Convenience wrapper: hash raw bytes and AddHashed.
    void Add(const void* data, std::size_t len);

    // PG: EstimateHyperLogLog — current cardinality estimate.
    uint64_t Estimate() const;

    int RegisterBits() const { return register_bits_; }
    std::size_t RegisterCount() const { return registers_.size(); }

    static constexpr int kDefaultRegisterBits = 14;

private:
    static uint64_t Hash(const void* data, std::size_t len);
    double Alpha() const;

    int register_bits_;
    int register_mask_;        // 2^register_bits - 1
    int hash_bits_remaining_;  // 64 - register_bits
    std::vector<uint8_t> registers_;
};

}  // namespace mytoydb::lib

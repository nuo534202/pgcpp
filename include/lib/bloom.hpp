// bloom.hpp — Bloom filter.
//
// Mirrors PostgreSQL's src/backend/lib/bloomfilter.c. A standard Bloom
// filter with k hash functions derived via double hashing:
//   h_i(x) = (h1(x) + i * h2(x)) mod m   for i in [0, k)
// where m is the bit array size. h1 and h2 are independent 64-bit hashes of
// the input bytes; using two seed values over the FNV-1a mixing primitive
// keeps the implementation self-contained and avoids dependency on a
// cryptographic hash library.

#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace pgcpp::lib {

class BloomFilter {
public:
    // Constructs a Bloom filter with the given bit count and k hash functions.
    // k must be >= 1; bit_count must be >= 1.
    BloomFilter(std::size_t bit_count, std::size_t k);

    // PG: bloom_add_element — insert bytes into the filter.
    void Add(const void* data, std::size_t len);

    // PG: bloom_has_element — membership test (may produce false positives,
    // never false negatives).
    bool Test(const void* data, std::size_t len) const;

    // PG: bloom_reset — clear all bits.
    void Reset();

    std::size_t BitCount() const { return bit_count_; }
    std::size_t HashCount() const { return k_; }

private:
    // Returns (h1, h2): two 64-bit hashes of the input bytes.
    static std::pair<uint64_t, uint64_t> Hash(const void* data, std::size_t len);

    std::vector<uint64_t> words_;  // packed bit storage (64 bits per word)
    std::size_t bit_count_;
    std::size_t k_;
};

}  // namespace pgcpp::lib

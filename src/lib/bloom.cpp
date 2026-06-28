// bloom.cpp — Bloom filter implementation.
//
// See mytoydb/lib/bloom.hpp for design notes. Storage is a packed bit array
// of 64-bit words; each Add/Test invocation computes (h1, h2) via two
// salted FNV-1a passes and derives k bit positions via double hashing.

#include "pgcpp/lib/bloom.hpp"

#include <cstring>

namespace mytoydb::lib {

namespace {

constexpr uint64_t kFnvPrime = 0x100000001b3ULL;
constexpr uint64_t kFnvOffsetH1 = 0xcbf29ce484222325ULL;
constexpr uint64_t kFnvOffsetH2 = 0x84222325cbf29ce4ULL;

uint64_t Fnv1a(const void* data, std::size_t len, uint64_t seed) {
    uint64_t hash = seed;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < len; ++i) {
        hash ^= static_cast<uint64_t>(bytes[i]);
        hash *= kFnvPrime;
    }
    return hash;
}

inline void SetBit(std::vector<uint64_t>& words, std::size_t bit_index) {
    std::size_t word = bit_index / 64;
    std::size_t bit = bit_index % 64;
    words[word] |= (uint64_t{1} << bit);
}

inline bool GetBit(const std::vector<uint64_t>& words, std::size_t bit_index) {
    std::size_t word = bit_index / 64;
    std::size_t bit = bit_index % 64;
    return (words[word] & (uint64_t{1} << bit)) != 0;
}

}  // namespace

BloomFilter::BloomFilter(std::size_t bit_count, std::size_t k)
    : bit_count_(bit_count == 0 ? 1 : bit_count), k_(k == 0 ? 1 : k) {
    std::size_t num_words = (bit_count_ + 63) / 64;
    words_.assign(num_words, 0);
}

void BloomFilter::Add(const void* data, std::size_t len) {
    auto [h1, h2] = Hash(data, len);
    if (h2 == 0) {
        h2 = 1;  // Avoid degenerate case where all positions collapse.
    }
    for (std::size_t i = 0; i < k_; ++i) {
        uint64_t pos = (h1 + i * h2) % bit_count_;
        SetBit(words_, static_cast<std::size_t>(pos));
    }
}

bool BloomFilter::Test(const void* data, std::size_t len) const {
    auto [h1, h2] = Hash(data, len);
    if (h2 == 0) {
        h2 = 1;
    }
    for (std::size_t i = 0; i < k_; ++i) {
        uint64_t pos = (h1 + i * h2) % bit_count_;
        if (!GetBit(words_, static_cast<std::size_t>(pos))) {
            return false;
        }
    }
    return true;
}

void BloomFilter::Reset() {
    for (uint64_t& w : words_) {
        w = 0;
    }
}

std::pair<uint64_t, uint64_t> BloomFilter::Hash(const void* data, std::size_t len) {
    uint64_t h1 = Fnv1a(data, len, kFnvOffsetH1);
    uint64_t h2 = Fnv1a(data, len, kFnvOffsetH2);
    return {h1, h2};
}

}  // namespace mytoydb::lib

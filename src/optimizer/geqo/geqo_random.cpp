// geqo_random.cpp — GEQO pseudo-random number generator.
//
// Converted from PostgreSQL 15's src/backend/optimizer/geqo/geqo_random.c.
//
// Implements a linear congruential generator with xorshift output mixing,
// providing the deterministic randomness GEQO needs for crossover, mutation,
// and selection. The generator is seeded once per query so that re-planning
// the same query yields the same join order.
#include "optimizer/geqo/geqo_random.hpp"

#include <cstdint>

namespace pgcpp::optimizer::geqo {

namespace {
// LCG step constants (Knuth's MMIX). These give a full-period generator over
// the 64-bit state space and good bit distribution.
constexpr uint64_t kLcgMult = 6364136223846793005ULL;
constexpr uint64_t kLcgAdd = 1442695040888963407ULL;
}  // namespace

int GeqoRandom::NextInt(int n) {
    if (n <= 0)
        return 0;
    state_ = state_ * kLcgMult + kLcgAdd;
    // Mix high bits (low bits of an LCG have poor entropy) via xorshift.
    uint64_t x = state_;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    // Map to [0, n) using the high 32 bits for better uniformity.
    return static_cast<int>((x >> 32) % static_cast<uint64_t>(n));
}

double GeqoRandom::NextDouble() {
    state_ = state_ * kLcgMult + kLcgAdd;
    // Use the top 53 bits to construct a double in [0, 1).
    uint64_t x = state_;
    x ^= x >> 11;
    return static_cast<double>(x >> 11) / static_cast<double>(1ULL << 53);
}

GeqoRandom& GeqoRng() {
    static GeqoRandom rng;
    return rng;
}

void SetGeqoSeed(uint64_t seed) {
    GeqoRng().Seed(seed);
}

}  // namespace pgcpp::optimizer::geqo

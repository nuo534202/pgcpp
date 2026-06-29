// geqo_random.h — GEQO pseudo-random number generator.
//
// Converted from PostgreSQL 15's src/backend/optimizer/geqo/geqo_random.c.
//
// GEQO needs reproducible randomness (so that re-planning the same query
// yields the same join order) and a tiny API surface. PostgreSQL implements
// a linear congruential generator (LCG) seeded once per query. pgcpp
// follows the same design: a deterministic, seedable LCG that produces
// uniformly distributed integers in [0, n).
#pragma once

#include <cstdint>

namespace pgcpp::optimizer::geqo {

// GeqoRandom — a small deterministic PRNG used by the genetic operators.
// The state is a single 64-bit word; advances use the classic LCG step
// x = x * 6364136223846793005 + 1442695040888963407 (Knuth's MMIX
// constants), and results are mixed with a xorshift to spread bits.
class GeqoRandom {
public:
    // Construct with a fixed seed (deterministic across runs).
    explicit GeqoRandom(uint64_t seed = 0x9E3779B97F4A7C15ULL) : state_(seed) {}

    // Re-seed the generator. Typically called once per GeqoSolve() call.
    void Seed(uint64_t seed) { state_ = seed; }

    // Return a uniformly distributed integer in [0, n). n must be > 0.
    int NextInt(int n);

    // Return a uniformly distributed double in [0.0, 1.0).
    double NextDouble();

    // Return the raw 64-bit state (for debugging/test assertions).
    uint64_t state() const { return state_; }

private:
    uint64_t state_;
};

// Global singleton accessor. PostgreSQL's GEQO uses module-level globals
// for the RNG; pgcpp wraps them in a function-local static to avoid
// global-construction-order hazards. Seed it once via SetGeqoSeed() at
// the start of each GeqoSolve() call.
GeqoRandom& GeqoRng();

// Re-seed the global RNG. Convenience wrapper around GeqoRng().Seed().
void SetGeqoSeed(uint64_t seed);

}  // namespace pgcpp::optimizer::geqo

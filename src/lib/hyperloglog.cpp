// hyperloglog.cpp — HyperLogLog cardinality estimator implementation.
//
// See mytoydb/lib/hyperloglog.hpp for the algorithm summary. The hash
// function uses a salted FNV-1a pass (same primitive as BloomFilter) so the
// module has no external hash dependency. The estimate follows Flajolet's
// small/large-range bias corrections.

#include "pgcpp/lib/hyperloglog.hpp"

#include <cmath>

namespace mytoydb::lib {

namespace {

constexpr uint64_t kFnvPrime = 0x100000001b3ULL;
constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ULL;

uint64_t Fnv1a(const void* data, std::size_t len) {
    uint64_t hash = kFnvOffset;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < len; ++i) {
        hash ^= static_cast<uint64_t>(bytes[i]);
        hash *= kFnvPrime;
    }
    return hash;
}

// Count leading zeros within a `bits`-bit field of `low_bits`, then add 1
// (the +1 accounts for the implicit "1" bit that terminates the zero run).
// Returns `bits + 1` when low_bits is zero (entire field is zero).
inline int Rho(uint64_t low_bits, int bits) {
    if (low_bits == 0) {
        return bits + 1;
    }
    int leading = 0;
    for (int i = bits - 1; i >= 0; --i) {
        if ((low_bits >> i) & 1u) {
            break;
        }
        ++leading;
    }
    return leading + 1;
}

}  // namespace

HyperLogLog::HyperLogLog(int register_bits) {
    if (register_bits < 4) {
        register_bits = 4;
    } else if (register_bits > 17) {
        register_bits = 17;
    }
    register_bits_ = register_bits;
    register_mask_ = (1 << register_bits_) - 1;
    hash_bits_remaining_ = 64 - register_bits_;
    registers_.assign(static_cast<std::size_t>(1) << register_bits_, 0);
}

void HyperLogLog::Reset() {
    for (uint8_t& r : registers_) {
        r = 0;
    }
}

void HyperLogLog::AddHashed(uint64_t hash) {
    // Use the LOW register_bits_ bits to select the register, and the
    // remaining HIGH bits to compute rho. FNV-1a (used by Hash) has poor
    // diffusion in the high bits for short, similar inputs (e.g. "item0"…
    // "item999"); its low bits are well-mixed by the multiplication step.
    // Using the low bits as the register index restores a uniform
    // distribution across all 2^p registers.
    int j = static_cast<int>(hash & register_mask_);
    uint64_t high_bits = hash >> register_bits_;
    int rho = Rho(high_bits, hash_bits_remaining_);
    if (rho > registers_[j]) {
        registers_[j] = static_cast<uint8_t>(rho);
    }
}

void HyperLogLog::Add(const void* data, std::size_t len) {
    AddHashed(Hash(data, len));
}

uint64_t HyperLogLog::Estimate() const {
    double m = static_cast<double>(registers_.size());
    double alpha_m = Alpha();
    double sum = 0.0;
    std::size_t zero_count = 0;
    for (uint8_t r : registers_) {
        sum += 1.0 / (static_cast<double>(uint64_t{1} << r));
        if (r == 0) {
            ++zero_count;
        }
    }
    double e = alpha_m * m * m / sum;

    // Small range correction: if E <= 2.5*m and there are zero registers,
    // use LinearCounting which is more accurate at low cardinality.
    if (e <= 2.5 * m && zero_count > 0) {
        e = m * std::log(m / static_cast<double>(zero_count));
    }
    // Large range correction: if E > 2^32 / 30, apply Flajolet's correction.
    constexpr double kTwo32 = 4294967296.0;
    if (e > kTwo32 / 30.0) {
        e = -kTwo32 * std::log(1.0 - e / kTwo32);
    }
    return static_cast<uint64_t>(e + 0.5);
}

double HyperLogLog::Alpha() const {
    double m = static_cast<double>(registers_.size());
    switch (registers_.size()) {
        case 16:
            return 0.673;
        case 32:
            return 0.697;
        case 64:
            return 0.709;
        default:
            return 0.7213 / (1.0 + 1.079 / m);
    }
}

uint64_t HyperLogLog::Hash(const void* data, std::size_t len) {
    return Fnv1a(data, len);
}

}  // namespace mytoydb::lib

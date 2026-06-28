// mvdistinct.cpp — multi-column ndistinct estimates.
//
// Converts the public API of PostgreSQL's src/backend/statistics/mvndistinct.c
// to C++20. For every column combination of size >= 2 the builder counts the
// distinct value-tuples in the sample and applies EstimateNDistinct (the
// Haas-Stefanski D1 estimator) to extrapolate to the population.
//
// The serialization format is a compact little-endian byte string:
//   'N' (magic)
//   uint32 count
//   count * (uint32 attrs, double ndistinct)
// matching the role (not the exact bytes) of PostgreSQL's bytea layout.

#include "mytoydb/statistics/mvdistinct.hpp"

#include <cstring>
#include <map>
#include <vector>

#include "mytoydb/statistics/mvdistinct_sample.hpp"

namespace mytoydb::statistics {

namespace {

// Per-combination distinct/singletons counts.
struct DistinctStats {
    int distinct = 0;
    int f1 = 0;  // number of value-tuples appearing exactly once
};

// Count distinct value-tuples for the given 0-based attribute indices.
// Treats each Datum as int64_t for the key (sufficient for the integer
// columns that dominate tests and ClickBench; a full type-aware comparison
// would require the operator-class lookup not yet converted).
DistinctStats CountDistinct(const StatsBuildData& data, const std::vector<int>& attr_indices) {
    std::map<std::vector<int64_t>, int> counts;
    for (int r = 0; r < data.nrows; ++r) {
        std::vector<int64_t> key;
        key.reserve(attr_indices.size());
        for (int idx : attr_indices) {
            key.push_back(static_cast<int64_t>(data.GetValue(r, idx)));
        }
        counts[key]++;
    }
    DistinctStats s;
    s.distinct = static_cast<int>(counts.size());
    for (const auto& [k, c] : counts) {
        if (c == 1)
            ++s.f1;
    }
    return s;
}

// All subsets of {0..n-1} of size >= 2, in ascending bit-mask order.
std::vector<std::vector<int>> AllSubsets(int n) {
    std::vector<std::vector<int>> result;
    for (int mask = 1; mask < (1 << n); ++mask) {
        std::vector<int> subset;
        for (int i = 0; i < n; ++i) {
            if (mask & (1 << i))
                subset.push_back(i);
        }
        if (subset.size() >= 2)
            result.push_back(subset);
    }
    return result;
}

// Build the attrs bitmask from 0-based attribute indices: bit i set means
// column attnums[i] (i.e. the i-th column in the build data) is included.
uint32_t AttrsBitmask(const std::vector<int>& attr_indices) {
    uint32_t mask = 0;
    for (int idx : attr_indices) {
        mask |= (1u << idx);
    }
    return mask;
}

}  // namespace

MVNDistinct BuildMVNDistinct(const StatsBuildData& data, double totalrows) {
    MVNDistinct result;
    int n = data.NumAttrs();
    if (n < 2 || data.nrows <= 0)
        return result;

    for (const auto& subset : AllSubsets(n)) {
        DistinctStats ds = CountDistinct(data, subset);
        double ndistinct =
            EstimateNDistinct(totalrows, static_cast<double>(data.nrows),
                              static_cast<double>(ds.distinct), static_cast<double>(ds.f1));
        MVNDistinctItem item;
        item.attrs = AttrsBitmask(subset);
        item.ndistinct = ndistinct;
        result.items.push_back(item);
    }
    return result;
}

double EstimateMVNDistinct(const MVNDistinct& nd, uint32_t attrs) {
    for (const auto& item : nd.items) {
        if (item.attrs == attrs)
            return item.ndistinct;
    }
    return -1.0;
}

std::string SerializeMVNDistinct(const MVNDistinct& nd) {
    std::string out;
    out.push_back('N');  // magic
    uint32_t count = static_cast<uint32_t>(nd.items.size());
    out.append(reinterpret_cast<const char*>(&count), sizeof(uint32_t));
    for (const auto& item : nd.items) {
        uint32_t attrs = item.attrs;
        double v = item.ndistinct;
        out.append(reinterpret_cast<const char*>(&attrs), sizeof(uint32_t));
        out.append(reinterpret_cast<const char*>(&v), sizeof(double));
    }
    return out;
}

MVNDistinct DeserializeMVNDistinct(std::string_view data) {
    MVNDistinct result;
    if (data.size() < 1 + sizeof(uint32_t) || data[0] != 'N')
        return result;
    uint32_t count;
    std::memcpy(&count, data.data() + 1, sizeof(uint32_t));
    size_t pos = 1 + sizeof(uint32_t);
    for (uint32_t i = 0; i < count; ++i) {
        if (pos + sizeof(uint32_t) + sizeof(double) > data.size())
            break;
        MVNDistinctItem item;
        std::memcpy(&item.attrs, data.data() + pos, sizeof(uint32_t));
        pos += sizeof(uint32_t);
        std::memcpy(&item.ndistinct, data.data() + pos, sizeof(double));
        pos += sizeof(double);
        result.items.push_back(item);
    }
    return result;
}

}  // namespace mytoydb::statistics

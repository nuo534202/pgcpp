// mvdependencies.cpp — soft functional dependencies.
//
// Converts the public API of PostgreSQL's src/backend/statistics/mvdependencies.c
// to C++20. For each ordered pair (a, b) of columns the builder groups the
// sample rows by the value of a and counts the distinct b-values within each
// group. The dependency degree is
//
//   degree = 1 - sum_g max(0, distinct_b_g - 1) / n
//
// which equals 1.0 when b is a perfect function of a (every group has exactly
// one distinct b-value) and decreases toward 0 as groups contain more
// conflicting b-values. This matches the spirit of PostgreSQL's degree
// measure (the fraction of rows consistent with a deterministic dependency).
//
// The serialization format is a compact little-endian byte string:
//   'F' (magic)
//   uint32 count
//   count * (uint32 nattrs, nattrs * (int32 attnum, uint8 is_eq), double degree)

#include "pgcpp/statistics/mvdependencies.hpp"

#include <cstring>
#include <map>
#include <set>
#include <vector>

#include "pgcpp/statistics/mvdistinct.hpp"  // StatsBuildData

namespace pgcpp::statistics {

MVDependencies BuildMVDependencies(const StatsBuildData& data) {
    MVDependencies result;
    int n = data.NumAttrs();
    if (n < 2 || data.nrows <= 0)
        return result;

    // For each ordered pair (a, b) with a != b, build the dependency a -> b.
    for (int a = 0; a < n; ++a) {
        for (int b = 0; b < n; ++b) {
            if (a == b)
                continue;

            // Group rows by the value of attribute a; for each group collect
            // the distinct values of attribute b.
            std::map<int64_t, std::set<int64_t>> groups;
            for (int r = 0; r < data.nrows; ++r) {
                int64_t a_val = static_cast<int64_t>(data.GetValue(r, a));
                int64_t b_val = static_cast<int64_t>(data.GetValue(r, b));
                groups[a_val].insert(b_val);
            }

            // degree = 1 - sum_g max(0, distinct_b_g - 1) / n
            double violations = 0.0;
            for (const auto& [a_val, b_vals] : groups) {
                int d = static_cast<int>(b_vals.size());
                if (d > 1)
                    violations += (d - 1);
            }
            double degree = 1.0 - violations / static_cast<double>(data.nrows);

            MVDependency dep;
            // determining attribute (left-hand side)
            dep.attributes.push_back({data.attnums[a], true});
            // dependent attribute (right-hand side)
            dep.attributes.push_back({data.attnums[b], true});
            dep.degree = degree;
            result.items.push_back(std::move(dep));
        }
    }
    return result;
}

const MVDependency* EstimateMVDependencies(const MVDependencies& deps, uint32_t attrs) {
    const MVDependency* best = nullptr;
    for (const auto& dep : deps.items) {
        // The determining attributes are all but the last entry.
        uint32_t det_mask = 0;
        for (size_t i = 0; i + 1 < dep.attributes.size(); ++i) {
            int attnum = dep.attributes[i].attnum;
            // attnum is 1-based; bit (attnum-1) marks that column.
            if (attnum >= 1 && attnum <= 32) {
                det_mask |= (1u << (attnum - 1));
            }
        }
        // The dependency applies only when its determining columns are all
        // available in `attrs` (i.e. det_mask is a subset of attrs).
        if ((det_mask & attrs) == det_mask) {
            if (best == nullptr || dep.degree > best->degree) {
                best = &dep;
            }
        }
    }
    return best;
}

std::string SerializeMVDependencies(const MVDependencies& deps) {
    std::string out;
    out.push_back('F');  // magic ("functional")
    uint32_t count = static_cast<uint32_t>(deps.items.size());
    out.append(reinterpret_cast<const char*>(&count), sizeof(uint32_t));
    for (const auto& dep : deps.items) {
        uint32_t nattrs = static_cast<uint32_t>(dep.attributes.size());
        out.append(reinterpret_cast<const char*>(&nattrs), sizeof(uint32_t));
        for (const auto& attr : dep.attributes) {
            // Encode attnum as int32 for a stable on-disk width.
            int32_t attnum = static_cast<int32_t>(attr.attnum);
            out.append(reinterpret_cast<const char*>(&attnum), sizeof(int32_t));
            char is_eq = attr.is_eq ? 1 : 0;
            out.push_back(is_eq);
        }
        double v = dep.degree;
        out.append(reinterpret_cast<const char*>(&v), sizeof(double));
    }
    return out;
}

MVDependencies DeserializeMVDependencies(std::string_view data) {
    MVDependencies result;
    if (data.size() < 1 + sizeof(uint32_t) || data[0] != 'F')
        return result;
    uint32_t count;
    std::memcpy(&count, data.data() + 1, sizeof(uint32_t));
    size_t pos = 1 + sizeof(uint32_t);
    for (uint32_t i = 0; i < count; ++i) {
        if (pos + sizeof(uint32_t) > data.size())
            break;
        MVDependency dep;
        uint32_t nattrs;
        std::memcpy(&nattrs, data.data() + pos, sizeof(uint32_t));
        pos += sizeof(uint32_t);
        for (uint32_t j = 0; j < nattrs; ++j) {
            if (pos + sizeof(int32_t) + 1 > data.size())
                return result;
            int32_t attnum;
            std::memcpy(&attnum, data.data() + pos, sizeof(int32_t));
            pos += sizeof(int32_t);
            bool is_eq = (data[pos] != 0);
            pos += 1;
            dep.attributes.push_back({static_cast<AttrNumber>(attnum), is_eq});
        }
        if (pos + sizeof(double) > data.size())
            return result;
        std::memcpy(&dep.degree, data.data() + pos, sizeof(double));
        pos += sizeof(double);
        result.items.push_back(std::move(dep));
    }
    return result;
}

}  // namespace pgcpp::statistics

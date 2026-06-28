// range.cpp — range types implementations.
//
// Mirrors PostgreSQL's utils/adt/rangetypes.c with a simplified RangeDatum
// struct that supports int4/int8/numeric/timestamp element types.

#include "pgcpp/types/range.hpp"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/memory_context.hpp"
#include "pgcpp/types/builtins.hpp"
#include "pgcpp/types/datetime.hpp"
#include "pgcpp/types/numeric.hpp"

namespace mytoydb::types {

using mytoydb::error::LogLevel;
using mytoydb::memory::palloc;

namespace {

char* PallocCString(std::string_view s) {
    char* buf = static_cast<char*>(palloc(s.size() + 1));
    if (!s.empty()) {
        std::memcpy(buf, s.data(), s.size());
    }
    buf[s.size()] = '\0';
    return buf;
}

void SkipSpaces(std::string_view s, std::size_t& it) {
    while (it < s.size() && std::isspace(static_cast<unsigned char>(s[it]))) {
        ++it;
    }
}

bool ParseToken(std::string_view s, std::size_t& it, char expected) {
    SkipSpaces(s, it);
    if (it < s.size() && s[it] == expected) {
        ++it;
        return true;
    }
    return false;
}

// Parse one bound token: a (possibly quoted) element literal followed by
// either ',' or the closing bracket.
bool ParseBoundLiteral(std::string_view s, std::size_t& it, std::string& out) {
    SkipSpaces(s, it);
    out.clear();
    if (it < s.size() && s[it] == '"') {
        ++it;
        while (it < s.size()) {
            char c = s[it];
            if (c == '\\' && it + 1 < s.size()) {
                out.push_back(s[it + 1]);
                it += 2;
                continue;
            }
            if (c == '"') {
                ++it;
                return true;
            }
            out.push_back(c);
            ++it;
        }
        return false;
    }
    while (it < s.size() && s[it] != ',' && s[it] != ')' && s[it] != ']') {
        out.push_back(s[it]);
        ++it;
    }
    std::size_t first = 0;
    while (first < out.size() && std::isspace(static_cast<unsigned char>(out[first]))) {
        ++first;
    }
    std::size_t last = out.size();
    while (last > first && std::isspace(static_cast<unsigned char>(out[last - 1]))) {
        --last;
    }
    out = out.substr(first, last - first);
    return true;
}

Datum ParseRangeLiteral(std::string_view s, uint32_t element_oid) {
    RangeDatum r{};
    std::size_t it = 0;
    SkipSpaces(s, it);
    if (it >= s.size()) {
        ereport(LogLevel::kError, "invalid range literal: empty input");
    }
    if (s.compare(it, 5, "empty") == 0) {
        r.flags = kRangeEmpty;
        it += 5;
        SkipSpaces(s, it);
        if (it != s.size()) {
            ereport(LogLevel::kError, "trailing garbage in range literal");
        }
        return MakeRangeDatum(r);
    }
    char open = s[it];
    bool lower_inclusive = (open == '[');
    if (open != '[' && open != '(') {
        ereport(LogLevel::kError, "invalid range literal: expected '[' or '('");
    }
    ++it;
    std::string lower_str;
    if (!ParseBoundLiteral(s, it, lower_str)) {
        ereport(LogLevel::kError, "invalid lower bound in range literal");
    }
    if (lower_str.empty()) {
        r.flags |= kRangeLowerIsNull;
    } else {
        switch (element_oid) {
            case kInt4Oid:
                r.lower = int4_in(lower_str.c_str());
                break;
            case kInt8Oid:
                r.lower = int8_in(lower_str.c_str());
                break;
            case kNumericOid:
                r.lower = numeric_in(lower_str.c_str());
                break;
            case kTimestampOid:
                r.lower = timestamp_in(lower_str.c_str());
                break;
            default:
                r.lower = int4_in(lower_str.c_str());
                break;
        }
    }
    if (!ParseToken(s, it, ',')) {
        ereport(LogLevel::kError, "expected ',' in range literal");
    }
    std::string upper_str;
    if (!ParseBoundLiteral(s, it, upper_str)) {
        ereport(LogLevel::kError, "invalid upper bound in range literal");
    }
    if (upper_str.empty()) {
        r.flags |= kRangeUpperIsNull;
    } else {
        switch (element_oid) {
            case kInt4Oid:
                r.upper = int4_in(upper_str.c_str());
                break;
            case kInt8Oid:
                r.upper = int8_in(upper_str.c_str());
                break;
            case kNumericOid:
                r.upper = numeric_in(upper_str.c_str());
                break;
            case kTimestampOid:
                r.upper = timestamp_in(upper_str.c_str());
                break;
            default:
                r.upper = int4_in(upper_str.c_str());
                break;
        }
    }
    SkipSpaces(s, it);
    if (it >= s.size()) {
        ereport(LogLevel::kError, "unterminated range literal");
    }
    char close = s[it];
    ++it;
    bool upper_inclusive = (close == ']');
    if (close != ']' && close != ')') {
        ereport(LogLevel::kError, "invalid range literal: expected ']' or ')'");
    }
    if (lower_inclusive) {
        r.flags |= kRangeLowerInclusive;
    }
    if (upper_inclusive) {
        r.flags |= kRangeUpperInclusive;
    }
    SkipSpaces(s, it);
    if (it != s.size()) {
        ereport(LogLevel::kError, "trailing garbage in range literal");
    }
    return MakeRangeDatum(r);
}

std::string FormatBound(Datum value, bool is_null, uint32_t element_oid) {
    if (is_null) {
        return "";
    }
    char* s = nullptr;
    switch (element_oid) {
        case kInt4Oid:
            s = int4_out(value);
            break;
        case kInt8Oid:
            s = int8_out(value);
            break;
        case kNumericOid:
            s = numeric_out(value);
            break;
        case kTimestampOid:
            s = timestamp_out(value);
            break;
        default:
            s = int4_out(value);
            break;
    }
    std::string out = (s == nullptr) ? "" : std::string(s);
    return out;
}

Datum GenericRangeIn(const char* str, uint32_t element_oid, const char* type_name) {
    if (str == nullptr) {
        ereport(LogLevel::kError,
                std::string("invalid input syntax for type ") + type_name + ": NULL");
    }
    return ParseRangeLiteral(str, element_oid);
}

std::string GenericRangeOut(const RangeDatum* r, uint32_t element_oid) {
    if (r->flags & kRangeEmpty) {
        return "empty";
    }
    std::string out;
    out.push_back((r->flags & kRangeLowerInclusive) ? '[' : '(');
    out += FormatBound(r->lower, (r->flags & kRangeLowerIsNull) != 0, element_oid);
    out.push_back(',');
    out += FormatBound(r->upper, (r->flags & kRangeUpperIsNull) != 0, element_oid);
    out.push_back((r->flags & kRangeUpperInclusive) ? ']' : ')');
    return out;
}

}  // namespace

Datum MakeRangeDatum(const RangeDatum& r) {
    auto* p = static_cast<RangeDatum*>(palloc(sizeof(RangeDatum)));
    *p = r;
    return reinterpret_cast<Datum>(p);
}

Datum int4range_in(const char* str) {
    return GenericRangeIn(str, kInt4Oid, "int4range");
}
char* int4range_out(Datum value) {
    return PallocCString(GenericRangeOut(DatumGetRange(value), kInt4Oid));
}

Datum int8range_in(const char* str) {
    return GenericRangeIn(str, kInt8Oid, "int8range");
}
char* int8range_out(Datum value) {
    return PallocCString(GenericRangeOut(DatumGetRange(value), kInt8Oid));
}

Datum numrange_in(const char* str) {
    return GenericRangeIn(str, kNumericOid, "numrange");
}
char* numrange_out(Datum value) {
    return PallocCString(GenericRangeOut(DatumGetRange(value), kNumericOid));
}

Datum tsrange_in(const char* str) {
    return GenericRangeIn(str, kTimestampOid, "tsrange");
}
char* tsrange_out(Datum value) {
    return PallocCString(GenericRangeOut(DatumGetRange(value), kTimestampOid));
}

int range_cmp_internal(uint32_t element_oid, Datum a, Datum b) {
    const auto* x = DatumGetRange(a);
    const auto* y = DatumGetRange(b);
    if ((x->flags & kRangeEmpty) && (y->flags & kRangeEmpty)) {
        return 0;
    }
    if (x->flags & kRangeEmpty) {
        return -1;
    }
    if (y->flags & kRangeEmpty) {
        return 1;
    }
    // Compare lower bounds.
    int lower_cmp = 0;
    if ((x->flags & kRangeLowerIsNull) && !(y->flags & kRangeLowerIsNull)) {
        return -1;
    }
    if (!(x->flags & kRangeLowerIsNull) && (y->flags & kRangeLowerIsNull)) {
        return 1;
    }
    if (!(x->flags & kRangeLowerIsNull)) {
        switch (element_oid) {
            case kInt4Oid:
                lower_cmp = int4_cmp(x->lower, y->lower);
                break;
            case kInt8Oid:
                lower_cmp = int8_cmp(x->lower, y->lower);
                break;
            case kNumericOid:
                lower_cmp = numeric_cmp(x->lower, y->lower);
                break;
            case kTimestampOid:
                lower_cmp = timestamp_cmp(x->lower, y->lower);
                break;
            default:
                lower_cmp = int4_cmp(x->lower, y->lower);
                break;
        }
        if (lower_cmp != 0) {
            return (lower_cmp < 0) ? -1 : 1;
        }
        if ((x->flags & kRangeLowerInclusive) != (y->flags & kRangeLowerInclusive)) {
            return (x->flags & kRangeLowerInclusive) ? -1 : 1;
        }
    }
    // Compare upper bounds.
    if ((x->flags & kRangeUpperIsNull) && !(y->flags & kRangeUpperIsNull)) {
        return 1;
    }
    if (!(x->flags & kRangeUpperIsNull) && (y->flags & kRangeUpperIsNull)) {
        return -1;
    }
    if (!(x->flags & kRangeUpperIsNull)) {
        int upper_cmp = 0;
        switch (element_oid) {
            case kInt4Oid:
                upper_cmp = int4_cmp(x->upper, y->upper);
                break;
            case kInt8Oid:
                upper_cmp = int8_cmp(x->upper, y->upper);
                break;
            case kNumericOid:
                upper_cmp = numeric_cmp(x->upper, y->upper);
                break;
            case kTimestampOid:
                upper_cmp = timestamp_cmp(x->upper, y->upper);
                break;
            default:
                upper_cmp = int4_cmp(x->upper, y->upper);
                break;
        }
        if (upper_cmp != 0) {
            return (upper_cmp < 0) ? -1 : 1;
        }
        if ((x->flags & kRangeUpperInclusive) != (y->flags & kRangeUpperInclusive)) {
            return (x->flags & kRangeUpperInclusive) ? 1 : -1;
        }
    }
    return 0;
}

Datum range_eq(Datum a, Datum b) {
    return BoolGetDatum(range_cmp_internal(kInt4Oid, a, b) == 0);
}
Datum range_ne(Datum a, Datum b) {
    return BoolGetDatum(range_cmp_internal(kInt4Oid, a, b) != 0);
}
Datum range_lt(Datum a, Datum b) {
    return BoolGetDatum(range_cmp_internal(kInt4Oid, a, b) < 0);
}
Datum range_le(Datum a, Datum b) {
    return BoolGetDatum(range_cmp_internal(kInt4Oid, a, b) <= 0);
}
Datum range_gt(Datum a, Datum b) {
    return BoolGetDatum(range_cmp_internal(kInt4Oid, a, b) > 0);
}
Datum range_ge(Datum a, Datum b) {
    return BoolGetDatum(range_cmp_internal(kInt4Oid, a, b) >= 0);
}

Datum range_contains_elem(Datum range, Datum elem) {
    const auto* r = DatumGetRange(range);
    if (r->flags & kRangeEmpty) {
        return BoolGetDatum(false);
    }
    bool lower_ok = true;
    if (!(r->flags & kRangeLowerIsNull)) {
        int c = int4_cmp(r->lower, elem);
        lower_ok = (r->flags & kRangeLowerInclusive) ? (c <= 0) : (c < 0);
    }
    bool upper_ok = true;
    if (!(r->flags & kRangeUpperIsNull)) {
        int c = int4_cmp(elem, r->upper);
        upper_ok = (r->flags & kRangeUpperInclusive) ? (c <= 0) : (c < 0);
    }
    return BoolGetDatum(lower_ok && upper_ok);
}

}  // namespace mytoydb::types

// numeric.cpp — arbitrary-precision decimal type.
//
// Simplified PostgreSQL numeric: a scaled int128 with a decimal scale.
// Sufficient for ClickBench AVG/SUM aggregation where exact byte-for-byte
// output is required for integer and simple-decimal results.

#include "mytoydb/types/numeric.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "mytoydb/common/error/elog.hpp"
#include "mytoydb/common/memory/memory_context.hpp"

namespace mytoydb::types {

using mytoydb::error::LogLevel;
using mytoydb::memory::palloc;

namespace {

// Allocate a palloc'd C string copy (null-terminated).
char* PallocCString(std::string_view s) {
    char* buf = static_cast<char*>(palloc(s.size() + 1));
    if (!s.empty()) {
        std::memcpy(buf, s.data(), s.size());
    }
    buf[s.size()] = '\0';
    return buf;
}

// Convert int128 to a decimal string (no leading zeros, except "0" for zero).
std::string Int128ToString(__int128 v) {
    if (v == 0) {
        return "0";
    }
    bool neg = v < 0;
    unsigned __int128 uv;
    if (neg) {
        // Compute abs(v) carefully to avoid overflow on INT128_MIN.
        uv = static_cast<unsigned __int128>(-(v + 1)) + 1;
    } else {
        uv = static_cast<unsigned __int128>(v);
    }
    char buf[42];  // 39 digits max + sign + null + slack
    int pos = sizeof(buf) - 1;
    buf[pos] = '\0';
    while (uv > 0) {
        buf[--pos] = static_cast<char>('0' + (uv % 10));
        uv /= 10;
    }
    std::string result(&buf[pos]);
    if (neg) {
        result.insert(result.begin(), '-');
    }
    return result;
}

// Parse a non-empty decimal digit string into an int128.
// Does not handle sign; caller handles leading +/-.
__int128 ParseDigits(std::string_view s) {
    __int128 result = 0;
    for (char c : s) {
        result = result * 10 + (c - '0');
    }
    return result;
}

// Compute 10^n as int128 (n must be in [0, 38]).
__int128 Pow10(int n) {
    __int128 r = 1;
    for (int i = 0; i < n; ++i) {
        r *= 10;
    }
    return r;
}

}  // namespace

// --- I/O ---

Datum numeric_in(const char* str) {
    if (str == nullptr) {
        ereport(LogLevel::kError, "invalid input syntax for type numeric: NULL");
    }
    std::string_view s(str);
    if (s.empty()) {
        ereport(LogLevel::kError, "invalid input syntax for type numeric: \"\"");
    }

    std::size_t i = 0;
    bool neg = false;
    if (s[0] == '+' || s[0] == '-') {
        neg = (s[0] == '-');
        ++i;
    }

    // Split into integer and fractional digit strings.
    std::string int_part;
    std::string frac_part;
    bool seen_dot = false;
    bool seen_digit = false;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (c == '.') {
            if (seen_dot) {
                ereport(LogLevel::kError,
                        "invalid input syntax for type numeric: \"" + std::string(str) + "\"");
            }
            seen_dot = true;
        } else if (c >= '0' && c <= '9') {
            seen_digit = true;
            if (seen_dot) {
                frac_part.push_back(c);
            } else {
                int_part.push_back(c);
            }
        } else {
            ereport(LogLevel::kError,
                    "invalid input syntax for type numeric: \"" + std::string(str) + "\"");
        }
    }
    if (!seen_digit) {
        ereport(LogLevel::kError,
                "invalid input syntax for type numeric: \"" + std::string(str) + "\"");
    }

    int32_t dscale = static_cast<int32_t>(frac_part.size());
    std::string digits = int_part + frac_part;
    __int128 value = ParseDigits(digits);
    if (neg) {
        value = -value;
    }

    return MakeNumericDatum(value, dscale);
}

char* numeric_out(Datum value) {
    const NumericData* n = DatumGetNumeric(value);
    __int128 v = n->value;
    int32_t dscale = n->dscale;

    bool neg = v < 0;
    std::string digits = Int128ToString(neg ? -v : v);

    std::string out;
    if (dscale <= 0) {
        // Pure integer output.
        out = digits;
    } else {
        // Pad with leading zeros so we have at least dscale+1 digits
        // (ensuring a "0" before the decimal point when needed).
        if (static_cast<int>(digits.size()) <= dscale) {
            digits.insert(digits.begin(), dscale - digits.size() + 1, '0');
        }
        int int_len = static_cast<int>(digits.size()) - dscale;
        out = digits.substr(0, int_len) + "." + digits.substr(int_len);
    }
    if (neg) {
        out.insert(out.begin(), '-');
    }
    return PallocCString(out);
}

// --- construction ---

Datum MakeNumericDatum(__int128 value, int32_t dscale) {
    NumericData* n = static_cast<NumericData*>(palloc(sizeof(NumericData)));
    n->value = value;
    n->dscale = dscale;
    return reinterpret_cast<Datum>(n);
}

Datum Int64ToNumeric(int64_t val) {
    return MakeNumericDatum(static_cast<__int128>(val), 0);
}

Datum Int32ToNumeric(int32_t val) {
    return MakeNumericDatum(static_cast<__int128>(val), 0);
}

Datum Float8ToNumeric(double val) {
    // Use 15 fractional digits; %.15f avoids scientific notation.
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.15f", val);
    return numeric_in(buf);
}

// --- arithmetic ---

// Promote both operands to the same dscale (max of the two) and return the
// aligned values and the common dscale.
struct AlignedPair {
    __int128 av;
    __int128 bv;
    int32_t dscale;
};

AlignedPair AlignScales(const NumericData* a, const NumericData* b) {
    AlignedPair r;
    r.dscale = a->dscale > b->dscale ? a->dscale : b->dscale;
    r.av = a->value * Pow10(r.dscale - a->dscale);
    r.bv = b->value * Pow10(r.dscale - b->dscale);
    return r;
}

Datum numeric_add(Datum a, Datum b) {
    const NumericData* na = DatumGetNumeric(a);
    const NumericData* nb = DatumGetNumeric(b);
    AlignedPair r = AlignScales(na, nb);
    return MakeNumericDatum(r.av + r.bv, r.dscale);
}

Datum numeric_sub(Datum a, Datum b) {
    const NumericData* na = DatumGetNumeric(a);
    const NumericData* nb = DatumGetNumeric(b);
    AlignedPair r = AlignScales(na, nb);
    return MakeNumericDatum(r.av - r.bv, r.dscale);
}

Datum numeric_mul(Datum a, Datum b) {
    const NumericData* na = DatumGetNumeric(a);
    const NumericData* nb = DatumGetNumeric(b);
    // Result dscale = sum of operand dscales (PostgreSQL rule).
    return MakeNumericDatum(na->value * nb->value, na->dscale + nb->dscale);
}

Datum numeric_div(Datum a, Datum b) {
    const NumericData* na = DatumGetNumeric(a);
    const NumericData* nb = DatumGetNumeric(b);
    if (nb->value == 0) {
        ereport(LogLevel::kError, "division by zero");
    }
    // Use max(a, b) + 8 decimal digits of precision (ClickBench-friendly).
    int32_t out_dscale = na->dscale > nb->dscale ? na->dscale : nb->dscale;
    out_dscale += 8;
    // a/b = (a.value / 10^a.dscale) / (b.value / 10^b.dscale)
    //     = (a.value * 10^b.dscale) / (b.value * 10^a.dscale)
    // To represent with out_dscale: result.value / 10^out_dscale = a/b
    // => result.value = a.value * 10^(b.dscale + out_dscale - a.dscale) / b.value
    int shift = nb->dscale + out_dscale - na->dscale;
    __int128 numerator = na->value * Pow10(shift);
    // Truncate toward zero (PostgreSQL default for numeric division).
    __int128 q = numerator / nb->value;
    return MakeNumericDatum(q, out_dscale);
}

// --- comparison ---

int numeric_cmp(Datum a, Datum b) {
    const NumericData* na = DatumGetNumeric(a);
    const NumericData* nb = DatumGetNumeric(b);
    AlignedPair r = AlignScales(na, nb);
    if (r.av < r.bv) {
        return -1;
    }
    if (r.av > r.bv) {
        return 1;
    }
    return 0;
}

Datum numeric_eq(Datum a, Datum b) {
    return BoolGetDatum(numeric_cmp(a, b) == 0);
}

Datum numeric_lt(Datum a, Datum b) {
    return BoolGetDatum(numeric_cmp(a, b) < 0);
}

// --- aggregation helpers ---

Datum numeric_accum(Datum trans, Datum newval) {
    return numeric_add(trans, newval);
}

Datum numeric_avg(Datum sum, int64_t count) {
    if (count == 0) {
        ereport(LogLevel::kError, "numeric_avg: division by zero (count is zero)");
    }
    Datum count_num = Int64ToNumeric(count);
    return numeric_div(sum, count_num);
}

// --- conversions ---

int64_t numeric_to_int64(Datum value) {
    const NumericData* n = DatumGetNumeric(value);
    __int128 v = n->value;
    if (n->dscale > 0) {
        v = v / Pow10(n->dscale);
    }
    // Range check (best-effort).
    if (v > static_cast<__int128>(INT64_MAX) || v < static_cast<__int128>(INT64_MIN)) {
        ereport(LogLevel::kError, "numeric value out of range for bigint");
    }
    return static_cast<int64_t>(v);
}

double numeric_to_float8(Datum value) {
    const NumericData* n = DatumGetNumeric(value);
    double d = static_cast<double>(n->value);
    if (n->dscale > 0) {
        d /= static_cast<double>(Pow10(n->dscale));
    }
    return d;
}

}  // namespace mytoydb::types

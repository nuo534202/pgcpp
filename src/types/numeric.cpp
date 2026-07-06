// numeric.cpp — arbitrary-precision decimal type (base-10000 variable-length).
//
// Mirrors PostgreSQL's utils/adt/numeric.c: NumericVar with base-10,000 digit
// array, supporting arbitrary precision. Each int16_t holds 4 decimal digits
// (NBASE = 10000). The sign, weight (position of digits[0]), and display
// scale (dscale) follow PG semantics:
//   value = sign * Σ(digits[i] * NBASE^(weight - i))  for i in [0, ndigits)
//
// digit position 0 = ones group (0..9999), -1 = first fractional group
// (0.0001..0.9999), 1 = NBASE group (10000..99990000), and so on.

#include "types/numeric.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "common/error/elog.hpp"
#include "common/memory/memory_context.hpp"

namespace pgcpp::types {

using pgcpp::error::LogLevel;
using pgcpp::memory::palloc;

namespace {

constexpr int kNBase = kNumericBase;

// Allocate a palloc'd C string copy (null-terminated).
char* PallocCString(std::string_view s) {
    char* buf = static_cast<char*>(palloc(s.size() + 1));
    if (!s.empty()) {
        std::memcpy(buf, s.data(), s.size());
    }
    buf[s.size()] = '\0';
    return buf;
}

// Working representation for arithmetic. Backed by std::vector for dynamic
// sizing; not exposed across the public API. Each function converts between
// NumericData (palloc'd, owned by memory context) and NumericVar (stack-local
// scratch) at its boundary.
struct NumericVar {
    std::vector<int16_t> digits;
    int weight = 0;
    int sign = kNumericPos;
    int dscale = 0;

    int NDigits() const { return static_cast<int>(digits.size()); }

    void Clear() {
        digits.clear();
        weight = 0;
        sign = kNumericPos;
        dscale = 0;
    }

    bool IsZero() const {
        for (int16_t d : digits) {
            if (d != 0)
                return false;
        }
        return true;
    }

    // Get the digit at logical position `pos` (0 = ones group, -1 = first
    // fractional group, 1 = NBASE group). Returns 0 outside the stored range.
    int16_t DigitAt(int pos) const {
        int idx = weight - pos;
        if (idx < 0 || idx >= NDigits())
            return 0;
        return digits[idx];
    }

    // Strip leading and trailing zero digit groups. After this, digits[0] is
    // the most significant non-zero group (or digits is empty for zero).
    void StripZeros() {
        while (!digits.empty() && digits.front() == 0) {
            digits.erase(digits.begin());
            weight--;
        }
        while (!digits.empty() && digits.back() == 0) {
            digits.pop_back();
        }
        if (digits.empty()) {
            sign = kNumericPos;
            weight = 0;
        }
    }

    // Ensure the digit array covers at least the positions implied by dscale.
    // Each fractional group covers 4 decimal places; we need ceil(dscale/4)
    // fractional groups. The least-significant stored position must be at
    // least -(frac_groups).
    void EnsureFracCoverage() {
        if (dscale <= 0)
            return;
        int frac_groups = (dscale + 3) / 4;
        int min_pos = -frac_groups;             // least significant position needed
        int last_pos = weight - NDigits() + 1;  // current least significant
        if (last_pos > min_pos) {
            int extra = last_pos - min_pos;
            digits.insert(digits.end(), extra, 0);
        }
    }

    // Copy from a NumericData Datum.
    void FromData(const NumericData* n) {
        digits.assign(n->digits, n->digits + n->ndigits);
        weight = n->weight;
        sign = n->sign;
        dscale = n->dscale;
    }

    // Allocate a fresh NumericData and copy this var's contents into it.
    NumericData* ToData() const {
        NumericData* n = static_cast<NumericData*>(palloc(sizeof(NumericData)));
        n->ndigits = NDigits();
        n->weight = weight;
        n->sign = sign;
        n->dscale = dscale;
        if (n->ndigits > 0) {
            n->digits = static_cast<int16_t*>(palloc(sizeof(int16_t) * n->ndigits));
            std::memcpy(n->digits, digits.data(), sizeof(int16_t) * n->ndigits);
        } else {
            n->digits = nullptr;
        }
        return n;
    }
};

// Compare absolute values of two vars. Returns -1, 0, 1.
int CompareAbs(const NumericVar& a, const NumericVar& b) {
    int a_max = a.weight;
    int b_max = b.weight;
    int max_pos = a_max > b_max ? a_max : b_max;
    int a_min = a.weight - a.NDigits() + 1;
    int b_min = b.weight - b.NDigits() + 1;
    int min_pos = a_min < b_min ? a_min : b_min;
    for (int pos = max_pos; pos >= min_pos; --pos) {
        int16_t ad = a.DigitAt(pos);
        int16_t bd = b.DigitAt(pos);
        if (ad < bd)
            return -1;
        if (ad > bd)
            return 1;
    }
    return 0;
}

// Compare signed values. Returns -1, 0, 1.
int CompareSigned(const NumericVar& a, const NumericVar& b) {
    bool a_zero = a.IsZero();
    bool b_zero = b.IsZero();
    if (a_zero && b_zero)
        return 0;
    if (a_zero)
        return b.sign == kNumericNeg ? 1 : -1;
    if (b_zero)
        return a.sign == kNumericNeg ? -1 : 1;
    if (a.sign != b.sign) {
        return a.sign == kNumericNeg ? -1 : 1;
    }
    int cmp = CompareAbs(a, b);
    return a.sign == kNumericNeg ? -cmp : cmp;
}

// Add absolute values: result = |a| + |b|.
void AddAbs(const NumericVar& a, const NumericVar& b, NumericVar* result) {
    int a_max = a.weight;
    int b_max = b.weight;
    int max_pos = a_max > b_max ? a_max : b_max;
    int a_min = a.weight - a.NDigits() + 1;
    int b_min = b.weight - b.NDigits() + 1;
    int min_pos = a_min < b_min ? a_min : b_min;

    std::vector<int16_t> out;
    out.reserve(max_pos - min_pos + 2);
    int carry = 0;
    for (int pos = min_pos; pos <= max_pos; ++pos) {
        int sum = a.DigitAt(pos) + b.DigitAt(pos) + carry;
        if (sum >= kNBase) {
            sum -= kNBase;
            carry = 1;
        } else {
            carry = 0;
        }
        out.push_back(static_cast<int16_t>(sum));
    }
    if (carry > 0) {
        out.push_back(1);
        max_pos++;
    }
    // out[0] corresponds to min_pos; we want digits[i] at position max_pos-i,
    // so reverse and set weight = max_pos.
    std::reverse(out.begin(), out.end());
    result->digits = std::move(out);
    result->weight = max_pos;
    result->sign = kNumericPos;
    result->StripZeros();
}

// Subtract absolute values: result = |a| - |b|. Caller must ensure |a| >= |b|.
void SubAbs(const NumericVar& a, const NumericVar& b, NumericVar* result) {
    int a_max = a.weight;
    int b_max = b.weight;
    int max_pos = a_max > b_max ? a_max : b_max;
    int a_min = a.weight - a.NDigits() + 1;
    int b_min = b.weight - b.NDigits() + 1;
    int min_pos = a_min < b_min ? a_min : b_min;

    std::vector<int16_t> out;
    out.reserve(max_pos - min_pos + 1);
    int borrow = 0;
    for (int pos = min_pos; pos <= max_pos; ++pos) {
        int diff = a.DigitAt(pos) - b.DigitAt(pos) - borrow;
        if (diff < 0) {
            diff += kNBase;
            borrow = 1;
        } else {
            borrow = 0;
        }
        out.push_back(static_cast<int16_t>(diff));
    }
    std::reverse(out.begin(), out.end());
    result->digits = std::move(out);
    result->weight = max_pos;
    result->sign = kNumericPos;
    result->StripZeros();
}

// Signed add: result = a + b.
void AddVar(const NumericVar& a, const NumericVar& b, NumericVar* result) {
    if (a.IsZero()) {
        *result = b;
        return;
    }
    if (b.IsZero()) {
        *result = a;
        return;
    }
    if (a.sign == b.sign) {
        AddAbs(a, b, result);
        result->sign = a.sign;
    } else {
        int cmp = CompareAbs(a, b);
        if (cmp == 0) {
            result->Clear();
            return;
        }
        if (cmp > 0) {
            SubAbs(a, b, result);
            result->sign = a.sign;
        } else {
            SubAbs(b, a, result);
            result->sign = b.sign;
        }
    }
    if (result->IsZero())
        result->sign = kNumericPos;
}

// Signed sub: result = a - b.
void SubVar(const NumericVar& a, const NumericVar& b, NumericVar* result) {
    NumericVar neg_b = b;
    if (!neg_b.IsZero()) {
        neg_b.sign = (b.sign == kNumericPos) ? kNumericNeg : kNumericPos;
    }
    AddVar(a, neg_b, result);
}

// Multiply: result = a * b.
void MulVar(const NumericVar& a, const NumericVar& b, NumericVar* result) {
    if (a.IsZero() || b.IsZero()) {
        result->Clear();
        return;
    }
    // The product's least-significant position is (a.weight - a.NDigits() + 1)
    // + (b.weight - b.NDigits() + 1). The most-significant position is
    // a.weight + b.weight + 1 (carry can extend one position up).
    int a_lo = a.weight - a.NDigits() + 1;
    int b_lo = b.weight - b.NDigits() + 1;
    int a_hi = a.weight;
    int b_hi = b.weight;
    int res_lo = a_lo + b_lo;
    int res_hi = a_hi + b_hi + 1;

    int n = res_hi - res_lo + 1;
    std::vector<int> accum(n + 1, 0);
    // accum[0] is res_hi, accum[n] is res_lo.
    for (int ai = 0; ai < a.NDigits(); ++ai) {
        int a_pos = a_hi - ai;                // position of a.digits[ai]
        int a_idx = res_hi - (a_pos + b_hi);  // accum index for a*b.digits[0]
        int32_t av = a.digits[ai];
        if (av == 0)
            continue;
        for (int bi = 0; bi < b.NDigits(); ++bi) {
            accum[a_idx + bi] += av * b.digits[bi];
        }
    }
    // Normalize carries from least significant upward.
    for (int i = n; i > 0; --i) {
        if (accum[i] >= kNBase) {
            accum[i - 1] += accum[i] / kNBase;
            accum[i] %= kNBase;
        }
    }
    // The top position may still overflow; let it carry into accum[0].
    if (accum[0] >= kNBase) {
        // Extend: prepend a new most-significant group.
        int carry = accum[0] / kNBase;
        accum[0] %= kNBase;
        accum.insert(accum.begin(), carry);
        res_hi++;
        n++;
    }
    // Build digits array (most significant first).
    std::vector<int16_t> out;
    out.reserve(n + 1);
    for (int i = 0; i <= n; ++i) {
        out.push_back(static_cast<int16_t>(accum[i]));
    }
    result->digits = std::move(out);
    result->weight = res_hi;
    result->sign = (a.sign == b.sign) ? kNumericPos : kNumericNeg;
    result->dscale = a.dscale + b.dscale;
    result->StripZeros();
    if (result->IsZero())
        result->sign = kNumericPos;
}

// Divide: result = a / b, truncating toward zero. Raises ereport(ERROR) on
// division by zero. Output dscale = max(a.dscale, b.dscale) + 8.
void DivVar(const NumericVar& a, const NumericVar& b, NumericVar* result) {
    if (b.IsZero()) {
        ereport(LogLevel::kError, "division by zero");
    }
    if (a.IsZero()) {
        result->Clear();
        result->dscale = (a.dscale > b.dscale ? a.dscale : b.dscale) + 8;
        return;
    }

    // To preserve fractional precision, scale the dividend by NBASE^extra_groups
    // (equivalent to 10^(4*extra_groups), which is >= the needed extra_dec
    // decimal digits). The quotient is then NBASE^extra_groups times the true
    // result, so we compensate by reducing the result weight by extra_groups.
    int out_dscale = (a.dscale > b.dscale ? a.dscale : b.dscale) + 8;
    int extra_dec = out_dscale - a.dscale + b.dscale;
    int extra_groups = (extra_dec + 3) / 4;
    if (extra_groups < 0)
        extra_groups = 0;

    // Scaled dividend: multiply |a| by NBASE^extra_groups by increasing weight
    // and appending zero digit groups to cover the new lower positions.
    NumericVar aa = a;
    aa.sign = kNumericPos;
    aa.digits.insert(aa.digits.end(), extra_groups, 0);
    aa.weight += extra_groups;

    NumericVar bb = b;
    bb.sign = kNumericPos;
    bb.StripZeros();

    // Result weight uses the ORIGINAL (pre-scale) weights: scaling multiplies
    // the quotient by NBASE^extra_groups, so we subtract extra_groups back.
    int res_weight = a.weight - bb.weight;
    int b_ndig = bb.NDigits();

    std::vector<int16_t> quot;

    if (b_ndig == 1) {
        // Single-digit divisor fast path: classic O(n) long division with
        // remainder tracking. Iterates MSB to LSB over the scaled dividend.
        int32_t divisor = bb.digits[0];
        int32_t rem = 0;
        for (int i = 0; i < aa.NDigits(); ++i) {
            int32_t cur = rem * kNBase + aa.digits[i];
            int32_t q = cur / divisor;
            rem = cur % divisor;
            quot.push_back(static_cast<int16_t>(q));
        }
    } else {
        // Multi-digit divisor: Knuth's Algorithm D with normalization so that
        // bb.digits[0] >= NBASE/2, which bounds the q estimate overshoot to 2.
        int32_t norm = kNBase / (bb.digits[0] + 1);

        // Scale dividend into work[] with one extra leading slot for carry.
        std::vector<int16_t> work(aa.NDigits() + 1, 0);
        int32_t carry = 0;
        for (int i = aa.NDigits() - 1; i >= 0; --i) {
            int32_t p = static_cast<int32_t>(aa.digits[i]) * norm + carry;
            work[i + 1] = static_cast<int16_t>(p % kNBase);
            carry = p / kNBase;
        }
        work[0] = static_cast<int16_t>(carry);

        // Scale divisor into div[] (b_ndig digits; leading carry is 0 because
        // norm*(bb.digits[0]+1) <= NBASE).
        std::vector<int16_t> div(b_ndig);
        carry = 0;
        for (int i = b_ndig - 1; i >= 0; --i) {
            int32_t p = static_cast<int32_t>(bb.digits[i]) * norm + carry;
            div[i] = static_cast<int16_t>(p % kNBase);
            carry = p / kNBase;
        }

        int work_size = static_cast<int>(work.size());
        for (int j = 0; j + b_ndig < work_size; ++j) {
            // Estimate q from the top two work digits and div[0].
            int32_t top = static_cast<int32_t>(work[j]) * kNBase + work[j + 1];
            int32_t q = top / div[0];
            if (q >= kNBase)
                q = kNBase - 1;

            // Adjust q down (at most 2 decrements after normalization).
            for (int adj = 0; adj < 2; ++adj) {
                int32_t borrow = 0;
                int32_t mcarry = 0;
                std::vector<int16_t> trial(b_ndig + 1);
                for (int i = 0; i <= b_ndig; ++i)
                    trial[i] = work[j + i];
                for (int i = b_ndig - 1; i >= 0; --i) {
                    int32_t p = static_cast<int32_t>(div[i]) * q + mcarry;
                    mcarry = p / kNBase;
                    int32_t sub = trial[i + 1] - (p % kNBase) - borrow;
                    if (sub < 0) {
                        sub += kNBase;
                        borrow = 1;
                    } else
                        borrow = 0;
                    trial[i + 1] = static_cast<int16_t>(sub);
                }
                int32_t top_sub = trial[0] - mcarry - borrow;
                if (top_sub < 0) {
                    q--;
                    continue;
                }
                trial[0] = static_cast<int16_t>(top_sub);
                for (int i = 0; i <= b_ndig; ++i)
                    work[j + i] = trial[i];
                break;
            }
            quot.push_back(static_cast<int16_t>(q));
        }
    }

    // Strip leading zero quotient digits, adjusting weight accordingly.
    while (!quot.empty() && quot[0] == 0) {
        quot.erase(quot.begin());
        res_weight--;
    }

    if (quot.empty()) {
        result->Clear();
        result->dscale = out_dscale;
        return;
    }
    result->digits = std::move(quot);
    result->weight = res_weight;
    result->sign = (a.sign == b.sign) ? kNumericPos : kNumericNeg;
    result->dscale = out_dscale;
    result->StripZeros();
    if (result->IsZero())
        result->sign = kNumericPos;
}

// Truncate v in-place to new_dscale decimal digits: mask the cutoff group
// (zeroing sub-group decimal digits below new_dscale) and drop all lower
// digit groups. Caller has already verified new_dscale < v->dscale.
void TruncToScale(NumericVar* v, int new_dscale) {
    int g = (new_dscale + 3) / 4;  // fractional groups to keep
    int cutoff_pos = -g;           // last kept fractional position

    // Mask the cutoff group if the cutoff falls mid-group.
    if (g > 0) {
        int keep_dec = new_dscale - 4 * (g - 1);  // decimal digits kept in group
        if (keep_dec < 4) {
            int drop_dec = 4 - keep_dec;
            int mask_divisor = 1;
            for (int i = 0; i < drop_dec; ++i)
                mask_divisor *= 10;
            int idx = v->weight - cutoff_pos;
            if (idx >= 0 && idx < v->NDigits()) {
                int16_t d = v->digits[idx];
                v->digits[idx] = static_cast<int16_t>((d / mask_divisor) * mask_divisor);
            }
        }
    }

    // Drop all digit groups below cutoff_pos.
    int keep_idx = v->weight - cutoff_pos + 1;
    if (keep_idx < 0)
        keep_idx = 0;
    if (keep_idx < static_cast<int>(v->digits.size())) {
        v->digits.resize(keep_idx);
    }
    v->dscale = new_dscale;
    v->StripZeros();
    if (v->IsZero())
        v->sign = kNumericPos;
}

// Round var to new_dscale decimal digits, half away from zero.
void RoundVar(NumericVar* v, int new_dscale) {
    if (new_dscale < 0)
        new_dscale = 0;
    if (new_dscale >= v->dscale) {
        return;  // no-op: rounding to same or larger scale preserves dscale
    }

    // Build rounding addend = 5 * 10^(-(new_dscale+1)), placed in the
    // appropriate base-10000 group. Adding this to |v| then truncating
    // implements round-half-away-from-zero.
    int g_add = (new_dscale + 4) / 4;                   // group of rounding digit
    int offset_add = new_dscale + 1 - 4 * (g_add - 1);  // 1..4 within group
    int addend_digit = 5;
    for (int i = 4 - offset_add; i > 0; --i)
        addend_digit *= 10;

    NumericVar addend;
    addend.digits.push_back(static_cast<int16_t>(addend_digit));
    addend.weight = -g_add;
    addend.sign = kNumericPos;
    addend.dscale = 0;

    // Add |addend| to |v|, preserving v's sign (half away from zero).
    NumericVar abs_v = *v;
    abs_v.sign = kNumericPos;
    NumericVar sum;
    AddAbs(abs_v, addend, &sum);
    sum.sign = v->sign;
    if (sum.IsZero())
        sum.sign = kNumericPos;
    *v = sum;

    // Truncate the rounded result to new_dscale.
    TruncToScale(v, new_dscale);
}

// Truncate var toward zero to new_dscale decimal digits.
void TruncVar(NumericVar* v, int new_dscale) {
    if (new_dscale < 0)
        new_dscale = 0;
    if (new_dscale >= v->dscale) {
        return;  // no-op
    }
    TruncToScale(v, new_dscale);
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

    // Split into integer and fractional digit strings (raw decimal digits).
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

    // Strip leading zeros from the integer part (but keep at least one digit).
    std::size_t first_nonzero = int_part.find_first_not_of('0');
    if (first_nonzero == std::string::npos) {
        int_part = "0";
    } else if (first_nonzero > 0) {
        int_part.erase(0, first_nonzero);
    }

    int dscale = static_cast<int>(frac_part.size());

    // Pad frac_part so its length is a multiple of 4 (one digit group).
    std::string frac = frac_part;
    while (frac.size() % 4 != 0)
        frac.push_back('0');

    // Build the full decimal digit string (integer + fractional). Strip leading
    // zeros for digit-group parsing; we will compute weight from group counts.
    std::string all = int_part + frac;
    // Strip leading zeros (but keep at least one char).
    std::size_t lnz = all.find_first_not_of('0');
    if (lnz == std::string::npos) {
        all = "0";
        lnz = 0;
    } else if (lnz > 0) {
        all.erase(0, lnz);
    }

    // Total fractional decimal digits (after padding) determines the position
    // of the ones group. The ones group is at position 0; the first fractional
    // group is at position -1.
    int frac_decimals = static_cast<int>(frac.size());  // padded
    int int_decimals = static_cast<int>(all.size()) - frac_decimals;

    // Number of full digit groups in the integer part (rounded up).
    int int_groups = (int_decimals + 3) / 4;
    int frac_groups = frac_decimals / 4;  // always a multiple of 4 after padding
    int total_groups = int_groups + frac_groups;

    // We'll parse digits left-to-right in groups of 4, but the leading group
    // may have fewer than 4 integer digits.
    // Reconstruct a left-padded digit string so each group has exactly 4 chars.
    std::string padded;
    int leading_pad = int_groups * 4 - int_decimals;
    if (leading_pad < 0)
        leading_pad = 0;
    padded.append(leading_pad, '0');
    padded.append(all);

    NumericVar v;
    v.dscale = dscale;
    v.sign = neg ? kNumericNeg : kNumericPos;
    v.weight = int_groups - 1;  // leftmost group is the most significant
    for (int g = 0; g < total_groups; ++g) {
        std::string group = padded.substr(g * 4, 4);
        int16_t d = static_cast<int16_t>(std::stoi(group));
        v.digits.push_back(d);
    }
    v.StripZeros();
    if (v.IsZero())
        v.sign = kNumericPos;
    return reinterpret_cast<Datum>(v.ToData());
}

char* numeric_out(Datum value) {
    const NumericData* n = DatumGetNumeric(value);
    NumericVar v;
    v.FromData(n);

    if (v.IsZero()) {
        // Render zero as "0" or "0.000..." depending on dscale.
        if (v.dscale <= 0) {
            return PallocCString("0");
        }
        std::string out = "0.";
        out.append(v.dscale, '0');
        return PallocCString(out);
    }

    // Build the decimal digit string (no leading zeros beyond what's needed
    // to cover the integer part, no sign).
    int int_groups = v.weight + 1;
    if (int_groups < 0)
        int_groups = 0;  // value < 1, no integer groups
    int frac_groups = 0;
    if (v.dscale > 0) {
        frac_groups = (v.dscale + 3) / 4;
    }
    int total_groups = int_groups + frac_groups;

    // Build a padded digit string of length total_groups * 4. We read each
    // digit group at its logical position and format with leading zeros.
    std::string digits_str;
    digits_str.reserve(total_groups * 4);
    for (int g = 0; g < total_groups; ++g) {
        int pos = (int_groups - 1) - g;
        int16_t d = v.DigitAt(pos);
        char buf[5];
        std::snprintf(buf, sizeof(buf), "%04d", static_cast<int>(d));
        digits_str.append(buf);
    }

    // The numeric value's decimal point sits after `int_groups * 4` digits.
    int int_digits = int_groups * 4;
    int frac_digits = static_cast<int>(digits_str.size()) - int_digits;
    // Trim excess fractional digits beyond dscale.
    if (frac_digits > v.dscale) {
        digits_str.erase(int_digits + v.dscale);
        frac_digits = v.dscale;
    }
    // If we don't yet have enough fractional digits to cover dscale, pad.
    if (frac_digits < v.dscale) {
        digits_str.append(v.dscale - frac_digits, '0');
    }

    // Strip leading zeros from the integer part (keep at least one digit).
    std::size_t lnz = digits_str.find_first_not_of('0');
    if (lnz == std::string::npos) {
        digits_str = "0";
    } else if (lnz > 0 && lnz < static_cast<std::size_t>(int_digits)) {
        digits_str.erase(0, lnz);
        int_digits -= static_cast<int>(lnz);
    } else if (lnz >= static_cast<std::size_t>(int_digits)) {
        // All integer digits are zero; collapse to "0".
        digits_str.erase(0, static_cast<std::size_t>(int_digits));
        digits_str.insert(digits_str.begin(), '0');
        int_digits = 1;
    }

    std::string out;
    if (v.sign == kNumericNeg)
        out.push_back('-');
    if (v.dscale > 0) {
        // Ensure we have at least int_digits + 1 + dscale chars.
        // Insert decimal point.
        if (static_cast<int>(digits_str.size()) < int_digits + v.dscale) {
            digits_str.append(int_digits + v.dscale - digits_str.size(), '0');
        }
        out.append(digits_str, 0, int_digits);
        out.push_back('.');
        out.append(digits_str, int_digits, v.dscale);
    } else {
        out.append(digits_str);
    }
    return PallocCString(out);
}

// --- construction ---

Datum MakeNumericDatum(const int16_t* digits, int ndigits, int weight, int sign, int dscale) {
    NumericVar v;
    v.weight = weight;
    v.sign = sign;
    v.dscale = dscale;
    if (ndigits > 0 && digits != nullptr) {
        v.digits.assign(digits, digits + ndigits);
    }
    v.StripZeros();
    if (v.IsZero())
        v.sign = kNumericPos;
    return reinterpret_cast<Datum>(v.ToData());
}

Datum Int64ToNumeric(int64_t val) {
    bool neg = val < 0;
    uint64_t uv = neg ? static_cast<uint64_t>(-(val + 1)) + 1 : static_cast<uint64_t>(val);
    NumericVar v;
    v.sign = neg ? kNumericNeg : kNumericPos;
    v.dscale = 0;
    v.weight = 0;
    if (uv == 0) {
        v.Clear();
        return reinterpret_cast<Datum>(v.ToData());
    }
    // Decompose into base-10000 groups, least significant first.
    std::vector<int16_t> groups;
    while (uv > 0) {
        groups.push_back(static_cast<int16_t>(uv % kNBase));
        uv /= kNBase;
    }
    // Reverse so digits[0] is most significant; weight = groups.size() - 1.
    v.digits.assign(groups.rbegin(), groups.rend());
    v.weight = static_cast<int>(groups.size()) - 1;
    return reinterpret_cast<Datum>(v.ToData());
}

Datum Int32ToNumeric(int32_t val) {
    return Int64ToNumeric(static_cast<int64_t>(val));
}

Datum Float8ToNumeric(double val) {
    // Use 15 fractional digits; %.15f avoids scientific notation.
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.15f", val);
    return numeric_in(buf);
}

// --- arithmetic ---

Datum numeric_add(Datum a, Datum b) {
    NumericVar va, vb, result;
    va.FromData(DatumGetNumeric(a));
    vb.FromData(DatumGetNumeric(b));
    AddVar(va, vb, &result);
    result.dscale = va.dscale > vb.dscale ? va.dscale : vb.dscale;
    result.EnsureFracCoverage();
    return reinterpret_cast<Datum>(result.ToData());
}

Datum numeric_sub(Datum a, Datum b) {
    NumericVar va, vb, result;
    va.FromData(DatumGetNumeric(a));
    vb.FromData(DatumGetNumeric(b));
    SubVar(va, vb, &result);
    result.dscale = va.dscale > vb.dscale ? va.dscale : vb.dscale;
    result.EnsureFracCoverage();
    return reinterpret_cast<Datum>(result.ToData());
}

Datum numeric_mul(Datum a, Datum b) {
    NumericVar va, vb, result;
    va.FromData(DatumGetNumeric(a));
    vb.FromData(DatumGetNumeric(b));
    MulVar(va, vb, &result);
    // MulVar already sets dscale = a.dscale + b.dscale.
    return reinterpret_cast<Datum>(result.ToData());
}

Datum numeric_div(Datum a, Datum b) {
    NumericVar va, vb, result;
    va.FromData(DatumGetNumeric(a));
    vb.FromData(DatumGetNumeric(b));
    DivVar(va, vb, &result);
    return reinterpret_cast<Datum>(result.ToData());
}

// --- rounding / truncation ---

Datum numeric_round(Datum value, int32_t new_dscale) {
    NumericVar v;
    v.FromData(DatumGetNumeric(value));
    RoundVar(&v, static_cast<int>(new_dscale));
    return reinterpret_cast<Datum>(v.ToData());
}

Datum numeric_trunc(Datum value, int32_t new_dscale) {
    NumericVar v;
    v.FromData(DatumGetNumeric(value));
    TruncVar(&v, static_cast<int>(new_dscale));
    return reinterpret_cast<Datum>(v.ToData());
}

Datum numeric_ceil(Datum value) {
    NumericVar v;
    v.FromData(DatumGetNumeric(value));
    if (v.dscale > 0) {
        // Truncate toward zero, then add 1 if there was a discarded nonzero
        // fraction and the original sign was positive.
        NumericVar truncated = v;
        TruncVar(&truncated, 0);
        // Compare |v| vs |truncated|: if they differ, a positive value rounds
        // up (away from zero); a negative value's truncation already is the
        // ceil (since truncation toward zero on a negative is the ceiling).
        int cmp = CompareAbs(v, truncated);
        if (cmp > 0 && v.sign == kNumericPos) {
            NumericVar one;
            one.digits.push_back(1);
            one.weight = 0;
            one.sign = kNumericPos;
            one.dscale = 0;
            NumericVar sum;
            AddAbs(truncated, one, &sum);
            sum.sign = kNumericPos;
            sum.dscale = 0;
            v = sum;
        } else {
            v = truncated;
            v.dscale = 0;
        }
    }
    return reinterpret_cast<Datum>(v.ToData());
}

Datum numeric_floor(Datum value) {
    NumericVar v;
    v.FromData(DatumGetNumeric(value));
    if (v.dscale > 0) {
        NumericVar truncated = v;
        TruncVar(&truncated, 0);
        int cmp = CompareAbs(v, truncated);
        if (cmp > 0 && v.sign == kNumericNeg) {
            NumericVar one;
            one.digits.push_back(1);
            one.weight = 0;
            one.sign = kNumericPos;
            one.dscale = 0;
            NumericVar sum;
            AddAbs(truncated, one, &sum);
            sum.sign = kNumericNeg;
            sum.dscale = 0;
            v = sum;
        } else {
            v = truncated;
            v.dscale = 0;
        }
    }
    return reinterpret_cast<Datum>(v.ToData());
}

Datum numeric_abs(Datum value) {
    NumericVar v;
    v.FromData(DatumGetNumeric(value));
    v.sign = kNumericPos;
    if (v.IsZero())
        v.sign = kNumericPos;
    return reinterpret_cast<Datum>(v.ToData());
}

// --- comparison ---

int numeric_cmp(Datum a, Datum b) {
    NumericVar va, vb;
    va.FromData(DatumGetNumeric(a));
    vb.FromData(DatumGetNumeric(b));
    return CompareSigned(va, vb);
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
    NumericVar v;
    v.FromData(DatumGetNumeric(value));
    // Truncate toward zero at the integer position.
    int64_t result = 0;
    for (int pos = v.weight; pos >= 0; --pos) {
        result = result * kNBase + v.DigitAt(pos);
    }
    if (v.sign == kNumericNeg)
        result = -result;
    return result;
}

double numeric_to_float8(Datum value) {
    NumericVar v;
    v.FromData(DatumGetNumeric(value));
    if (v.IsZero())
        return 0.0;
    double result = 0.0;
    // Accumulate all digit groups as a single integer, MSB first. Each group
    // is at position (weight - i); the LSB is at the least-significant stored
    // position. The accumulated integer is value * NBASE^frac_groups, where
    // frac_groups = ceil(dscale / 4).
    for (int pos = v.weight; pos >= v.weight - v.NDigits() + 1; --pos) {
        result = result * kNBase + v.DigitAt(pos);
    }
    // Divide by NBASE^(number of fractional groups) to recover the true value.
    int frac_groups = (v.dscale + 3) / 4;
    for (int i = 0; i < frac_groups; ++i) {
        result /= static_cast<double>(kNBase);
    }
    if (v.sign == kNumericNeg)
        result = -result;
    return result;
}

}  // namespace pgcpp::types

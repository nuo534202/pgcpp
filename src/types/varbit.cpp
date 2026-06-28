// varbit.cpp — bit / varbit type implementations.
//
// Mirrors PostgreSQL's utils/adt/varbit.c with a simplified in-memory VarBit
// struct.

#include "pgcpp/types/varbit.hpp"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/memory_context.hpp"

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

VarBit* AllocVarBit(int32_t bit_len) {
    std::size_t byte_len = (static_cast<std::size_t>(bit_len) + 7) / 8;
    auto* p = static_cast<VarBit*>(palloc(sizeof(VarBit) + byte_len));
    p->bit_len = bit_len;
    std::memset(p->bits, 0, byte_len);
    return p;
}

Datum ParseBits(std::string_view s, int32_t typmod, bool fixed_length, const char* type_name) {
    int32_t in_bits = static_cast<int32_t>(s.size());
    // typmod <= 0 means "no typmod specified" (PostgreSQL passes -1 in this
    // case); we use the input length as the target.
    int32_t target = (typmod > 0) ? typmod : in_bits;
    if (typmod > 0 && in_bits > typmod) {
        ereport(LogLevel::kError, std::string("bit string too long for ") + type_name);
    }
    if (fixed_length && typmod > 0 && in_bits < typmod) {
        // bit(n): pad with zeros up to n bits (handled by AllocVarBit zeroing).
    }
    VarBit* v = AllocVarBit(target);
    for (int i = 0; i < in_bits; ++i) {
        char c = s[i];
        if (c != '0' && c != '1') {
            ereport(LogLevel::kError, std::string("invalid character in bit string for ") +
                                          type_name + ": \"" + std::string(s) + "\"");
        }
        if (c == '1') {
            int byte = i / 8;
            int bit = 7 - (i % 8);
            v->bits[byte] |= static_cast<uint8_t>(1u << bit);
        }
    }
    return reinterpret_cast<Datum>(v);
}

}  // namespace

Datum MakeVarBitDatum(const uint8_t* bits, int32_t bit_len) {
    VarBit* v = AllocVarBit(bit_len);
    std::size_t byte_len = (static_cast<std::size_t>(bit_len) + 7) / 8;
    if (bits != nullptr && byte_len > 0) {
        std::memcpy(v->bits, bits, byte_len);
        // Mask off trailing bits.
        int extra = bit_len % 8;
        if (extra != 0) {
            uint8_t mask = static_cast<uint8_t>(0xff << (8 - extra));
            v->bits[byte_len - 1] &= mask;
        }
    }
    return reinterpret_cast<Datum>(v);
}

Datum bit_in(const char* str, int32_t typmod) {
    if (str == nullptr) {
        ereport(LogLevel::kError, "invalid input syntax for type bit: NULL");
    }
    return ParseBits(str, typmod, true, "bit");
}

Datum varbit_in(const char* str, int32_t typmod) {
    if (str == nullptr) {
        ereport(LogLevel::kError, "invalid input syntax for type varbit: NULL");
    }
    return ParseBits(str, typmod, false, "varbit");
}

Datum bit_in_default(const char* str) {
    return bit_in(str, -1);
}
Datum varbit_in_default(const char* str) {
    return varbit_in(str, -1);
}

char* bit_out(Datum value) {
    const auto* v = DatumGetVarBit(value);
    std::string s;
    s.reserve(static_cast<std::size_t>(v->bit_len));
    for (int i = 0; i < v->bit_len; ++i) {
        int byte = i / 8;
        int bit = 7 - (i % 8);
        s.push_back((v->bits[byte] & (1u << bit)) ? '1' : '0');
    }
    return PallocCString(s);
}

char* varbit_out(Datum value) {
    return bit_out(value);
}

int varbit_cmp(Datum a, Datum b) {
    const auto* x = DatumGetVarBit(a);
    const auto* y = DatumGetVarBit(b);
    int n = (x->bit_len < y->bit_len) ? x->bit_len : y->bit_len;
    int bytes = n / 8;
    int bits = n % 8;
    if (bytes > 0) {
        int cmp = std::memcmp(x->bits, y->bits, static_cast<std::size_t>(bytes));
        if (cmp != 0) {
            return (cmp < 0) ? -1 : 1;
        }
    }
    if (bits > 0) {
        uint8_t mask = static_cast<uint8_t>(0xff << (8 - bits));
        uint8_t xb = x->bits[bytes] & mask;
        uint8_t yb = y->bits[bytes] & mask;
        if (xb != yb) {
            return (xb < yb) ? -1 : 1;
        }
    }
    if (x->bit_len != y->bit_len) {
        return (x->bit_len < y->bit_len) ? -1 : 1;
    }
    return 0;
}

Datum varbit_eq(Datum a, Datum b) {
    return BoolGetDatum(varbit_cmp(a, b) == 0);
}
Datum varbit_ne(Datum a, Datum b) {
    return BoolGetDatum(varbit_cmp(a, b) != 0);
}
Datum varbit_lt(Datum a, Datum b) {
    return BoolGetDatum(varbit_cmp(a, b) < 0);
}
Datum varbit_le(Datum a, Datum b) {
    return BoolGetDatum(varbit_cmp(a, b) <= 0);
}
Datum varbit_gt(Datum a, Datum b) {
    return BoolGetDatum(varbit_cmp(a, b) > 0);
}
Datum varbit_ge(Datum a, Datum b) {
    return BoolGetDatum(varbit_cmp(a, b) >= 0);
}

Datum bit_length(Datum value) {
    const auto* v = DatumGetVarBit(value);
    return Int32GetDatum(v->bit_len);
}

Datum bit_and(Datum a, Datum b) {
    const auto* x = DatumGetVarBit(a);
    const auto* y = DatumGetVarBit(b);
    int n = (x->bit_len < y->bit_len) ? x->bit_len : y->bit_len;
    VarBit* v = AllocVarBit(n);
    int bytes = n / 8;
    int bits = n % 8;
    for (int i = 0; i < bytes; ++i) {
        v->bits[i] = x->bits[i] & y->bits[i];
    }
    if (bits > 0) {
        uint8_t mask = static_cast<uint8_t>(0xff << (8 - bits));
        v->bits[bytes] = (x->bits[bytes] & y->bits[bytes]) & mask;
    }
    return reinterpret_cast<Datum>(v);
}

Datum bit_or(Datum a, Datum b) {
    const auto* x = DatumGetVarBit(a);
    const auto* y = DatumGetVarBit(b);
    int n = (x->bit_len < y->bit_len) ? x->bit_len : y->bit_len;
    VarBit* v = AllocVarBit(n);
    int bytes = n / 8;
    int bits = n % 8;
    for (int i = 0; i < bytes; ++i) {
        v->bits[i] = x->bits[i] | y->bits[i];
    }
    if (bits > 0) {
        uint8_t mask = static_cast<uint8_t>(0xff << (8 - bits));
        v->bits[bytes] = (x->bits[bytes] | y->bits[bytes]) & mask;
    }
    return reinterpret_cast<Datum>(v);
}

Datum bit_xor(Datum a, Datum b) {
    const auto* x = DatumGetVarBit(a);
    const auto* y = DatumGetVarBit(b);
    int n = (x->bit_len < y->bit_len) ? x->bit_len : y->bit_len;
    VarBit* v = AllocVarBit(n);
    int bytes = n / 8;
    int bits = n % 8;
    for (int i = 0; i < bytes; ++i) {
        v->bits[i] = x->bits[i] ^ y->bits[i];
    }
    if (bits > 0) {
        uint8_t mask = static_cast<uint8_t>(0xff << (8 - bits));
        v->bits[bytes] = (x->bits[bytes] ^ y->bits[bytes]) & mask;
    }
    return reinterpret_cast<Datum>(v);
}

Datum bit_not(Datum a) {
    const auto* x = DatumGetVarBit(a);
    VarBit* v = AllocVarBit(x->bit_len);
    int bytes = (x->bit_len + 7) / 8;
    for (int i = 0; i < bytes; ++i) {
        v->bits[i] = static_cast<uint8_t>(~x->bits[i]);
    }
    int bits = x->bit_len % 8;
    if (bits != 0) {
        uint8_t mask = static_cast<uint8_t>(0xff << (8 - bits));
        v->bits[bytes - 1] &= mask;
    }
    return reinterpret_cast<Datum>(v);
}

}  // namespace mytoydb::types

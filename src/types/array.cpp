// array.cpp — array type implementation (PostgreSQL utils/adt/arrayfuncs.c).
//
// Simplified in-memory representation. Parses PostgreSQL array literals of the
// form '{e1,e2,e3}' (1-D) and '{{1,2},{3,4}}' (n-D).

#include "pgcpp/types/array.hpp"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/memory_context.hpp"
#include "pgcpp/types/builtins.hpp"

namespace mytoydb::types {

using mytoydb::error::LogLevel;
using mytoydb::memory::MemoryContext;
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

// Skip ASCII whitespace.
void SkipSpaces(std::string_view s, std::size_t& it) {
    while (it < s.size() && std::isspace(static_cast<unsigned char>(s[it]))) {
        ++it;
    }
}

// Parse a single element token starting at `it`. The element ends at the next
// top-level ',' or '}' (we don't support nested-element separators). The
// returned token has surrounding whitespace and surrounding double quotes
// stripped, and embedded escape sequences ("" and \) unescaped. Returns true
// on success, false on malformed input. `null_token` is set to true when the
// element is the bare word NULL (case-insensitive).
bool ParseElement(std::string_view s, std::size_t& it, std::string& token, bool& null_token) {
    token.clear();
    null_token = false;
    SkipSpaces(s, it);
    if (it >= s.size()) {
        return false;
    }
    if (s[it] == '"') {
        ++it;
        while (it < s.size()) {
            char c = s[it];
            if (c == '\\' && it + 1 < s.size()) {
                token.push_back(s[it + 1]);
                it += 2;
                continue;
            }
            if (c == '"') {
                if (it + 1 < s.size() && s[it + 1] == '"') {
                    token.push_back('"');
                    it += 2;
                    continue;
                }
                ++it;
                break;
            }
            token.push_back(c);
            ++it;
        }
        return true;
    }
    // Unquoted: scan until separator or close.
    while (it < s.size() && s[it] != ',' && s[it] != '}') {
        token.push_back(s[it]);
        ++it;
    }
    // Trim surrounding whitespace.
    std::size_t first = 0;
    while (first < token.size() && std::isspace(static_cast<unsigned char>(token[first]))) {
        ++first;
    }
    std::size_t last = token.size();
    while (last > first && std::isspace(static_cast<unsigned char>(token[last - 1]))) {
        --last;
    }
    token = token.substr(first, last - first);
    if (token.empty()) {
        null_token = true;
        return true;
    }
    if (token.size() == 4) {
        bool is_null = true;
        for (int i = 0; i < 4; ++i) {
            if (std::toupper(static_cast<unsigned char>(token[i])) != "NULL"[i]) {
                is_null = false;
                break;
            }
        }
        if (is_null) {
            null_token = true;
            token.clear();
        }
    }
    return true;
}

Datum ParseElementValue(uint32_t element_oid, const std::string& token) {
    switch (element_oid) {
        case kInt2Oid:
            return int2_in(token.c_str());
        case kInt4Oid:
            return int4_in(token.c_str());
        case kInt8Oid:
            return int8_in(token.c_str());
        case kFloat8Oid:
            return float8_in(token.c_str());
        case kTextOid:
        case kVarcharOid:
            return text_in(token.c_str());
        case kBoolOid:
            return bool_in(token.c_str());
        default:
            // Default: store as text.
            return text_in(token.c_str());
    }
}

bool ParseArrayRecursive(std::string_view s, std::size_t& it, uint32_t element_oid, ArrayData* arr,
                         int depth) {
    SkipSpaces(s, it);
    if (it >= s.size() || s[it] != '{') {
        return false;
    }
    ++it;
    SkipSpaces(s, it);
    // Handle empty array '{}' or empty sub-array '{}'.
    if (it < s.size() && s[it] == '}') {
        ++it;
        return true;
    }
    while (true) {
        SkipSpaces(s, it);
        if (it < s.size() && s[it] == '{') {
            // Nested array. Recurse.
            if (!ParseArrayRecursive(s, it, element_oid, arr, depth + 1)) {
                return false;
            }
            if (static_cast<int>(arr->ndims) <= depth) {
                arr->ndims = depth + 1;
            }
        } else {
            std::string token;
            bool null_token = false;
            if (!ParseElement(s, it, token, null_token)) {
                return false;
            }
            if (null_token) {
                arr->values.push_back(0);
                arr->nulls.push_back(true);
            } else {
                arr->values.push_back(ParseElementValue(element_oid, token));
                arr->nulls.push_back(false);
            }
        }
        SkipSpaces(s, it);
        if (it >= s.size()) {
            return false;
        }
        if (s[it] == ',') {
            ++it;
            continue;
        }
        if (s[it] == '}') {
            ++it;
            return true;
        }
        return false;
    }
}

}  // namespace

Datum MakeArrayDatum(uint32_t element_oid, const std::vector<Datum>& values) {
    auto* arr = static_cast<ArrayData*>(palloc(sizeof(ArrayData)));
    new (arr) ArrayData();
    MemoryContext* ctx = mytoydb::memory::GetCurrentMemoryContext();
    if (ctx != nullptr) {
        ctx->RegisterDestructor(arr, [](void* p) { static_cast<ArrayData*>(p)->~ArrayData(); });
    }
    arr->element_oid = element_oid;
    arr->ndims = 1;
    arr->dims.push_back(static_cast<int32_t>(values.size()));
    arr->lower_bounds.push_back(1);
    arr->values = values;
    arr->nulls.assign(values.size(), false);
    return reinterpret_cast<Datum>(arr);
}

Datum array_in(const char* str, uint32_t element_oid, int32_t /*typmod*/) {
    if (str == nullptr) {
        ereport(LogLevel::kError, "invalid input syntax for type array: NULL");
    }
    std::string_view s(str);
    std::size_t it = 0;
    auto* arr = static_cast<ArrayData*>(palloc(sizeof(ArrayData)));
    new (arr) ArrayData();
    MemoryContext* ctx = mytoydb::memory::GetCurrentMemoryContext();
    if (ctx != nullptr) {
        ctx->RegisterDestructor(arr, [](void* p) { static_cast<ArrayData*>(p)->~ArrayData(); });
    }
    arr->element_oid = element_oid;
    arr->ndims = 0;
    if (!ParseArrayRecursive(s, it, element_oid, arr, 0)) {
        ereport(LogLevel::kError,
                "invalid input syntax for type array: \"" + std::string(str) + "\"");
    }
    if (arr->ndims == 0) {
        arr->ndims = 1;
    }
    arr->dims.assign(arr->ndims, 0);
    // Compute simple 1-D length for tests; multi-D dimensions are an approximation.
    arr->dims[0] = static_cast<int32_t>(arr->values.size());
    arr->lower_bounds.assign(arr->ndims, 1);
    return reinterpret_cast<Datum>(arr);
}

namespace {

char* FormatElement(uint32_t element_oid, Datum value, bool is_null) {
    if (is_null) {
        return nullptr;
    }
    switch (element_oid) {
        case kInt2Oid:
            return int2_out(value);
        case kInt4Oid:
            return int4_out(value);
        case kInt8Oid:
            return int8_out(value);
        case kFloat8Oid:
            return float8_out(value);
        case kTextOid:
        case kVarcharOid:
            return text_out(value);
        case kBoolOid:
            return bool_out(value);
        default:
            return text_out(value);
    }
}

bool NeedsQuoting(const std::string& s) {
    if (s.empty()) {
        return true;
    }
    for (char c : s) {
        if (c == ',' || c == '{' || c == '}' || c == '"' || c == '\\' ||
            std::isspace(static_cast<unsigned char>(c))) {
            return true;
        }
    }
    return false;
}

std::string QuoteIfNeeded(const std::string& s) {
    if (!NeedsQuoting(s)) {
        return s;
    }
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

}  // namespace

char* array_out(Datum value) {
    const auto* arr = DatumGetArray(value);
    std::string out = "{";
    for (std::size_t i = 0; i < arr->values.size(); ++i) {
        if (i > 0) {
            out.push_back(',');
        }
        if (arr->nulls[i]) {
            out += "NULL";
            continue;
        }
        char* formatted = FormatElement(arr->element_oid, arr->values[i], false);
        std::string token = (formatted == nullptr) ? "NULL" : std::string(formatted);
        out += QuoteIfNeeded(token);
    }
    out.push_back('}');
    return PallocCString(out);
}

int array_cmp(Datum a, Datum b) {
    const auto* x = DatumGetArray(a);
    const auto* y = DatumGetArray(b);
    if (x->element_oid != y->element_oid) {
        return (x->element_oid < y->element_oid) ? -1 : 1;
    }
    std::size_t n = (x->values.size() < y->values.size()) ? x->values.size() : y->values.size();
    for (std::size_t i = 0; i < n; ++i) {
        // NULL sorts before non-NULL.
        if (x->nulls[i] && y->nulls[i]) {
            continue;
        }
        if (x->nulls[i]) {
            return -1;
        }
        if (y->nulls[i]) {
            return 1;
        }
        Datum xv = x->values[i];
        Datum yv = y->values[i];
        int cmp = 0;
        switch (x->element_oid) {
            case kInt4Oid:
                cmp = int4_cmp(xv, yv);
                break;
            case kInt8Oid:
                cmp = int8_cmp(xv, yv);
                break;
            case kFloat8Oid:
                cmp = float8_cmp(xv, yv);
                break;
            case kTextOid:
            case kVarcharOid:
                cmp = text_cmp(xv, yv);
                break;
            default:
                cmp = text_cmp(xv, yv);
                break;
        }
        if (cmp != 0) {
            return (cmp < 0) ? -1 : 1;
        }
    }
    if (x->values.size() != y->values.size()) {
        return (x->values.size() < y->values.size()) ? -1 : 1;
    }
    return 0;
}

Datum array_eq(Datum a, Datum b) {
    return BoolGetDatum(array_cmp(a, b) == 0);
}
Datum array_ne(Datum a, Datum b) {
    return BoolGetDatum(array_cmp(a, b) != 0);
}
Datum array_lt(Datum a, Datum b) {
    return BoolGetDatum(array_cmp(a, b) < 0);
}

Datum array_length(Datum array, Datum dim) {
    const auto* arr = DatumGetArray(array);
    int32_t d = DatumGetInt32(dim);
    if (d < 1 || d > arr->ndims) {
        return BoolGetDatum(false);  // NULL marker
    }
    return Int32GetDatum(arr->dims[d - 1]);
}

Datum array_append(Datum array, Datum element) {
    const auto* src = DatumGetArray(array);
    auto* dst = static_cast<ArrayData*>(palloc(sizeof(ArrayData)));
    new (dst) ArrayData();
    MemoryContext* ctx = mytoydb::memory::GetCurrentMemoryContext();
    if (ctx != nullptr) {
        ctx->RegisterDestructor(dst, [](void* p) { static_cast<ArrayData*>(p)->~ArrayData(); });
    }
    dst->element_oid = src->element_oid;
    dst->ndims = 1;
    dst->dims.push_back(static_cast<int32_t>(src->values.size() + 1));
    dst->lower_bounds.push_back(1);
    dst->values = src->values;
    dst->values.push_back(element);
    dst->nulls = src->nulls;
    dst->nulls.push_back(false);
    return reinterpret_cast<Datum>(dst);
}

Datum array_ndims(Datum array) {
    const auto* arr = DatumGetArray(array);
    return Int32GetDatum(arr->ndims);
}

}  // namespace mytoydb::types

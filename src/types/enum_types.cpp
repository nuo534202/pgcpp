// enum_types.cpp — enum type implementations.
//
// Mirrors PostgreSQL's utils/adt/enum.c with an in-memory registry.

#include "pgcpp/types/enum_types.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

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

struct EnumCatalog {
    // For each enum type OID, ordered (sortorder, label) and label->sortorder.
    std::map<uint32_t, std::map<int32_t, std::string>> by_sortorder;
    std::map<uint32_t, std::unordered_map<std::string, int32_t>> by_label;
    std::map<uint32_t, int32_t> next_sortorder;
};

EnumCatalog& GetCatalog() {
    static EnumCatalog c;
    return c;
}

std::mutex& GetMutex() {
    static std::mutex m;
    return m;
}

}  // namespace

void EnumRegisterLabel(uint32_t enum_type_oid, const char* label) {
    if (label == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> guard(GetMutex());
    auto& cat = GetCatalog();
    auto& label_map = cat.by_label[enum_type_oid];
    auto it = label_map.find(label);
    if (it != label_map.end()) {
        return;  // already registered
    }
    int32_t sortorder = cat.next_sortorder[enum_type_oid];
    if (sortorder == 0) {
        sortorder = 1;
    }
    cat.next_sortorder[enum_type_oid] = sortorder + 1;
    cat.by_sortorder[enum_type_oid][sortorder] = label;
    label_map[label] = sortorder;
}

int32_t EnumLookupLabel(uint32_t enum_type_oid, const char* label) {
    if (label == nullptr) {
        return -1;
    }
    std::lock_guard<std::mutex> guard(GetMutex());
    const auto& cat = GetCatalog();
    auto it = cat.by_label.find(enum_type_oid);
    if (it == cat.by_label.end()) {
        return -1;
    }
    auto lit = it->second.find(label);
    if (lit == it->second.end()) {
        return -1;
    }
    return lit->second;
}

const char* EnumLookupSortorder(uint32_t enum_type_oid, int32_t sortorder) {
    std::lock_guard<std::mutex> guard(GetMutex());
    const auto& cat = GetCatalog();
    auto it = cat.by_sortorder.find(enum_type_oid);
    if (it == cat.by_sortorder.end()) {
        return nullptr;
    }
    auto sit = it->second.find(sortorder);
    if (sit == it->second.end()) {
        return nullptr;
    }
    return sit->second.c_str();
}

void EnumResetRegistry() {
    std::lock_guard<std::mutex> guard(GetMutex());
    auto& cat = GetCatalog();
    cat.by_sortorder.clear();
    cat.by_label.clear();
    cat.next_sortorder.clear();
}

Datum enum_in(const char* str, uint32_t enum_type_oid) {
    if (str == nullptr) {
        ereport(LogLevel::kError, "invalid input syntax for type enum: NULL");
    }
    int32_t sortorder = EnumLookupLabel(enum_type_oid, str);
    if (sortorder < 0) {
        ereport(LogLevel::kError, "invalid enum label: \"" + std::string(str) + "\"");
    }
    return Int32GetDatum(sortorder);
}

char* enum_out(Datum value, uint32_t enum_type_oid) {
    int32_t sortorder = DatumGetInt32(value);
    const char* label = EnumLookupSortorder(enum_type_oid, sortorder);
    if (label == nullptr) {
        return PallocCString("?");
    }
    return PallocCString(label);
}

int enum_cmp(Datum a, Datum b) {
    int32_t x = DatumGetInt32(a);
    int32_t y = DatumGetInt32(b);
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

Datum enum_eq(Datum a, Datum b) {
    return BoolGetDatum(enum_cmp(a, b) == 0);
}
Datum enum_lt(Datum a, Datum b) {
    return BoolGetDatum(enum_cmp(a, b) < 0);
}

}  // namespace mytoydb::types

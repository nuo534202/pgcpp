// oid_types.cpp — implementations for the OID family (oid, regproc, regclass, etc.)
//
// Mirrors PostgreSQL's utils/adt/oid.c and regproc.c.

#include "pgcpp/types/oid_types.hpp"

#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

// One registry per catalog. Using std::map for stable behaviour and easy lookup.
struct RegTable {
    std::map<uint32_t, std::string> oid_to_name;
    std::unordered_map<std::string, uint32_t> name_to_oid;
};

RegTable& GetTable(RegCatalog cat) {
    static RegTable tables[6] = {};
    return tables[static_cast<int>(cat)];
}

std::mutex& GetMutex() {
    static std::mutex m;
    return m;
}

// Parse a string as an OID (numeric) or return false on failure.
bool ParseNumericOid(std::string_view s, uint32_t& out) {
    if (s.empty()) {
        return false;
    }
    errno = 0;
    char* endptr = nullptr;
    std::string buf(s);
    unsigned long val = std::strtoul(buf.c_str(), &endptr, 10);
    if (errno == ERANGE || *endptr != '\0' || endptr == buf.c_str()) {
        return false;
    }
    if (val > UINT32_MAX) {
        return false;
    }
    out = static_cast<uint32_t>(val);
    return true;
}

Datum GenericOidIn(const char* str) {
    if (str == nullptr) {
        ereport(LogLevel::kError, "invalid input syntax for type oid: NULL");
    }
    uint32_t val = 0;
    if (!ParseNumericOid(str, val)) {
        ereport(LogLevel::kError,
                "invalid input syntax for type oid: \"" + std::string(str) + "\"");
    }
    return Datum(val);
}

Datum GenericRegIn(const char* str, RegCatalog cat, const char* type_name) {
    if (str == nullptr) {
        ereport(LogLevel::kError,
                std::string("invalid input syntax for type ") + type_name + ": NULL");
    }
    std::string_view s(str);
    if (s.empty()) {
        return Datum(0);
    }
    // Try numeric first.
    uint32_t val = 0;
    if (ParseNumericOid(s, val)) {
        return Datum(val);
    }
    // Lookup by name (case-sensitive).
    std::lock_guard<std::mutex> guard(GetMutex());
    const auto& table = GetTable(cat);
    auto it = table.name_to_oid.find(std::string(s));
    if (it == table.name_to_oid.end()) {
        ereport(LogLevel::kError, std::string("no matching entry for type ") + type_name + ": \"" +
                                      std::string(s) + "\"");
    }
    return Datum(it->second);
}

char* GenericRegOut(Datum value, RegCatalog cat) {
    uint32_t oid = static_cast<uint32_t>(value);
    std::lock_guard<std::mutex> guard(GetMutex());
    const auto& table = GetTable(cat);
    auto it = table.oid_to_name.find(oid);
    if (it != table.oid_to_name.end()) {
        return PallocCString(it->second);
    }
    return PallocCString(std::to_string(oid));
}

}  // namespace

// --- oid ---
Datum oid_in(const char* str) {
    return GenericOidIn(str);
}
char* oid_out(Datum value) {
    return PallocCString(std::to_string(static_cast<uint32_t>(value)));
}

int oid_cmp(Datum a, Datum b) {
    uint32_t x = static_cast<uint32_t>(a);
    uint32_t y = static_cast<uint32_t>(b);
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

Datum oid_eq(Datum a, Datum b) {
    return BoolGetDatum(oid_cmp(a, b) == 0);
}
Datum oid_lt(Datum a, Datum b) {
    return BoolGetDatum(oid_cmp(a, b) < 0);
}
Datum oid_gt(Datum a, Datum b) {
    return BoolGetDatum(oid_cmp(a, b) > 0);
}

// --- reg* family ---
Datum regproc_in(const char* str) {
    return GenericRegIn(str, RegCatalog::kProc, "regproc");
}
char* regproc_out(Datum value) {
    return GenericRegOut(value, RegCatalog::kProc);
}

Datum regprocedure_in(const char* str) {
    return GenericRegIn(str, RegCatalog::kProc, "regprocedure");
}
char* regprocedure_out(Datum value) {
    return GenericRegOut(value, RegCatalog::kProc);
}

Datum regoper_in(const char* str) {
    return GenericRegIn(str, RegCatalog::kOper, "regoper");
}
char* regoper_out(Datum value) {
    return GenericRegOut(value, RegCatalog::kOper);
}

Datum regoperator_in(const char* str) {
    return GenericRegIn(str, RegCatalog::kOper, "regoperator");
}
char* regoperator_out(Datum value) {
    return GenericRegOut(value, RegCatalog::kOper);
}

Datum regclass_in(const char* str) {
    return GenericRegIn(str, RegCatalog::kClass, "regclass");
}
char* regclass_out(Datum value) {
    return GenericRegOut(value, RegCatalog::kClass);
}

Datum regtype_in(const char* str) {
    return GenericRegIn(str, RegCatalog::kType, "regtype");
}
char* regtype_out(Datum value) {
    return GenericRegOut(value, RegCatalog::kType);
}

Datum regnamespace_in(const char* str) {
    return GenericRegIn(str, RegCatalog::kNamespace, "regnamespace");
}
char* regnamespace_out(Datum value) {
    return GenericRegOut(value, RegCatalog::kNamespace);
}

Datum regrole_in(const char* str) {
    return GenericRegIn(str, RegCatalog::kRole, "regrole");
}
char* regrole_out(Datum value) {
    return GenericRegOut(value, RegCatalog::kRole);
}

// --- shared comparison operators ---
Datum oidfamily_eq(Datum a, Datum b) {
    return BoolGetDatum(oidfamily_cmp(a, b) == 0);
}
Datum oidfamily_ne(Datum a, Datum b) {
    return BoolGetDatum(oidfamily_cmp(a, b) != 0);
}
Datum oidfamily_lt(Datum a, Datum b) {
    return BoolGetDatum(oidfamily_cmp(a, b) < 0);
}
Datum oidfamily_le(Datum a, Datum b) {
    return BoolGetDatum(oidfamily_cmp(a, b) <= 0);
}
Datum oidfamily_gt(Datum a, Datum b) {
    return BoolGetDatum(oidfamily_cmp(a, b) > 0);
}
Datum oidfamily_ge(Datum a, Datum b) {
    return BoolGetDatum(oidfamily_cmp(a, b) >= 0);
}
int oidfamily_cmp(Datum a, Datum b) {
    return oid_cmp(a, b);
}

// --- registry helpers ---
void RegisterRegName(RegCatalog cat, uint32_t oid, const char* name) {
    if (name == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> guard(GetMutex());
    auto& table = GetTable(cat);
    table.oid_to_name[oid] = std::string(name);
    table.name_to_oid[std::string(name)] = oid;
}

const char* LookupRegName(RegCatalog cat, uint32_t oid) {
    std::lock_guard<std::mutex> guard(GetMutex());
    const auto& table = GetTable(cat);
    auto it = table.oid_to_name.find(oid);
    if (it == table.oid_to_name.end()) {
        return nullptr;
    }
    return it->second.c_str();
}

uint32_t LookupRegOid(RegCatalog cat, const char* name) {
    if (name == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> guard(GetMutex());
    const auto& table = GetTable(cat);
    auto it = table.name_to_oid.find(std::string(name));
    if (it == table.name_to_oid.end()) {
        return 0;
    }
    return it->second;
}

void ResetRegCatalogs() {
    std::lock_guard<std::mutex> guard(GetMutex());
    for (int i = 0; i < 6; ++i) {
        GetTable(static_cast<RegCatalog>(i)).oid_to_name.clear();
        GetTable(static_cast<RegCatalog>(i)).name_to_oid.clear();
    }
}

}  // namespace mytoydb::types

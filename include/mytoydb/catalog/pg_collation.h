#pragma once

#include <cstdint>
#include <string>

#include "mytoydb/catalog/catalog.h"

namespace mytoydb::catalog {

// FormData_pg_collation — C++ equivalent of PostgreSQL's catalog/pg_collation.h.
//
// Each row describes a collation (locale for string comparison/sorting).

// collprovider values (PostgreSQL 'char' codes).
enum class CollProvider : char {
    kDefault = 'd',   // default collation
    kIcu = 'i',       // ICU collation
    kLibc = 'c',      // libc collation
};

inline const char* CollProviderName(CollProvider p) {
    switch (p) {
        case CollProvider::kIcu:  return "icu";
        case CollProvider::kLibc: return "libc";
        default: return "???";
    }
}

struct FormData_pg_collation {
    Oid oid = kInvalidOid;              // collation OID
    std::string collname;                // collation name
    Oid collnamespace = kInvalidOid;     // namespace OID
    Oid collowner = kInvalidOid;         // owner OID
    CollProvider collprovider = CollProvider::kDefault;
    bool collisdeterministic = true;
    int32_t collencoding = -1;           // encoding (-1 = "all")
    std::string collcollate;             // LC_COLLATE setting
    std::string collctype;               // LC_CTYPE setting
    std::string colliculocale;           // ICU locale ID
    std::string collversion;             // provider-dependent version
};

using Form_pg_collation = FormData_pg_collation*;

// Well-known collation OIDs (from PostgreSQL's pg_collation_d.h).
constexpr Oid kDefaultCollationOid = 100;     // "default" collation
constexpr Oid kC_COLLATION_OID = 950;         // "C" collation
constexpr Oid kPOSIX_COLLATION_OID = 951;     // "POSIX" collation

}  // namespace mytoydb::catalog

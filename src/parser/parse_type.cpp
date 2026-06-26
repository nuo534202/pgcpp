// parse_type.cpp — Type lookup helpers for parse analysis.
//
// Converted from PostgreSQL 15's src/backend/parser/parse_type.c.
// Provides typenameTypeId() and related helpers to resolve type names
// to OIDs during parse analysis.
#include "mytoydb/parser/parse_type.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

#include "mytoydb/catalog/catalog.hpp"
#include "mytoydb/catalog/pg_type.hpp"
#include "mytoydb/catalog/syscache.hpp"
#include "mytoydb/common/error/elog.hpp"
#include "mytoydb/types/datum.hpp"

namespace mytoydb::parser {

using mytoydb::catalog::Catalog;
using mytoydb::catalog::FormData_pg_type;
using mytoydb::catalog::GetCatalog;
using mytoydb::catalog::GetSysCache;
using mytoydb::catalog::kInvalidOid;
using mytoydb::catalog::Oid;

// UNKNOWNOID — PostgreSQL's OID for the "unknown" type (705).
static constexpr Oid kUnknownOid = 705;

// ---------------------------------------------------------------------------
// Built-in type name to OID mapping for common types.
// In a full implementation, this would use the catalog/syscache.
// ---------------------------------------------------------------------------

namespace {

struct BuiltinType {
    const char* name;
    Oid oid;
    int16_t typlen;
    bool typbyval;
};

// Common built-in types needed for ClickBench.
const BuiltinType kBuiltinTypes[] = {
    {"bool", mytoydb::types::kBoolOid, 1, true},
    {"boolean", mytoydb::types::kBoolOid, 1, true},
    {"int2", mytoydb::types::kInt2Oid, 2, true},
    {"smallint", mytoydb::types::kInt2Oid, 2, true},
    {"int4", mytoydb::types::kInt4Oid, 4, true},
    {"integer", mytoydb::types::kInt4Oid, 4, true},
    {"int", mytoydb::types::kInt4Oid, 4, true},
    {"int8", mytoydb::types::kInt8Oid, 8, true},
    {"bigint", mytoydb::types::kInt8Oid, 8, true},
    {"float4", mytoydb::types::kFloat4Oid, 4, true},
    {"real", mytoydb::types::kFloat4Oid, 4, true},
    {"float8", mytoydb::types::kFloat8Oid, 8, true},
    {"double", mytoydb::types::kFloat8Oid, 8, true},
    {"double precision", mytoydb::types::kFloat8Oid, 8, true},
    {"text", mytoydb::types::kTextOid, -1, false},
    {"varchar", mytoydb::types::kVarcharOid, -1, false},
    {"char", mytoydb::types::kTextOid, -1, false},
    {"bpchar", mytoydb::types::kTextOid, -1, false},
    {"date", mytoydb::types::kDateOid, 4, true},
    {"timestamp", mytoydb::types::kTimestampOid, 8, true},
    {"unknown", kUnknownOid, -2, false},
};

constexpr int kBuiltinTypeCount = sizeof(kBuiltinTypes) / sizeof(kBuiltinTypes[0]);

const BuiltinType* FindBuiltinType(const std::string& name) {
    // PostgreSQL folds identifiers to lowercase (unless quoted). Match that
    // behavior so type names like "BIGINT", "Date", "TIMESTAMP" resolve.
    std::string lower_name = name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    for (int i = 0; i < kBuiltinTypeCount; ++i) {
        if (lower_name == kBuiltinTypes[i].name) {
            return &kBuiltinTypes[i];
        }
    }
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// typenameTypeId — resolve a type name to its OID.
// Returns InvalidOid if the type is not found.
Oid typenameTypeId(const std::string& typename_name) {
    // PostgreSQL folds identifiers to lowercase. Apply the same folding
    // before all lookups so "BIGINT", "Date", etc. resolve correctly.
    std::string lower_name = typename_name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // First check built-in types
    const BuiltinType* bt = FindBuiltinType(lower_name);
    if (bt != nullptr) {
        return bt->oid;
    }

    // Then check the catalog
    if (GetCatalog() != nullptr) {
        const FormData_pg_type* type_row = GetCatalog()->GetTypeByName(lower_name);
        if (type_row != nullptr) {
            return type_row->oid;
        }
    }

    // Then check syscache
    if (GetSysCache() != nullptr) {
        const FormData_pg_type* type_row = GetSysCache()->SearchTypeByName(lower_name, 0);
        if (type_row != nullptr) {
            return type_row->oid;
        }
    }

    return kInvalidOid;
}

// typeTypeId — get the OID from a pg_type row.
Oid typeTypeId(const FormData_pg_type* type_tuple) {
    return type_tuple ? type_tuple->oid : kInvalidOid;
}

// typeLen — get the typlen from a pg_type row.
int16_t typeLen(const FormData_pg_type* type_tuple) {
    return type_tuple ? type_tuple->typlen : 0;
}

// typeByVal — get the typbyval from a pg_type row.
bool typeByVal(const FormData_pg_type* type_tuple) {
    return type_tuple ? type_tuple->typbyval : false;
}

// get_typname — look up a type name by OID.
std::string get_typname(Oid type_oid) {
    // Check built-in types
    for (int i = 0; i < kBuiltinTypeCount; ++i) {
        if (type_oid == kBuiltinTypes[i].oid) {
            return kBuiltinTypes[i].name;
        }
    }

    // Check catalog
    if (GetCatalog() != nullptr) {
        const FormData_pg_type* type_row = GetCatalog()->GetTypeByOid(type_oid);
        if (type_row != nullptr) {
            return type_row->typname;
        }
    }

    return "unknown";
}

// get_typlen — get the typlen for a type OID.
int16_t get_typlen(Oid type_oid) {
    for (int i = 0; i < kBuiltinTypeCount; ++i) {
        if (type_oid == kBuiltinTypes[i].oid) {
            return kBuiltinTypes[i].typlen;
        }
    }
    if (GetCatalog() != nullptr) {
        const FormData_pg_type* type_row = GetCatalog()->GetTypeByOid(type_oid);
        if (type_row != nullptr) {
            return type_row->typlen;
        }
    }
    return 0;
}

// get_typbyval — get the typbyval for a type OID.
bool get_typbyval(Oid type_oid) {
    for (int i = 0; i < kBuiltinTypeCount; ++i) {
        if (type_oid == kBuiltinTypes[i].oid) {
            return kBuiltinTypes[i].typbyval;
        }
    }
    if (GetCatalog() != nullptr) {
        const FormData_pg_type* type_row = GetCatalog()->GetTypeByOid(type_oid);
        if (type_row != nullptr) {
            return type_row->typbyval;
        }
    }
    return false;
}

// type_is_numeric — is this a numeric type (int, float)?
bool type_is_numeric(Oid type_oid) {
    return type_oid == mytoydb::types::kInt2Oid || type_oid == mytoydb::types::kInt4Oid ||
           type_oid == mytoydb::types::kInt8Oid || type_oid == mytoydb::types::kFloat4Oid ||
           type_oid == mytoydb::types::kFloat8Oid;
}

// type_is_string — is this a string type (text, varchar, unknown)?
bool type_is_string(Oid type_oid) {
    return type_oid == mytoydb::types::kTextOid || type_oid == mytoydb::types::kVarcharOid ||
           type_oid == kUnknownOid;
}

// type_is_datetime — is this a date/time type?
bool type_is_datetime(Oid type_oid) {
    return type_oid == mytoydb::types::kDateOid || type_oid == mytoydb::types::kTimestampOid;
}

}  // namespace mytoydb::parser

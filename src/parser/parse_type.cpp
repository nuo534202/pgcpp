// parse_type.cpp — Type lookup helpers for parse analysis.
//
// Converted from PostgreSQL 15's src/backend/parser/parse_type.c.
// Provides typenameTypeId() and related helpers to resolve type names
// to OIDs during parse analysis.
#include "pgcpp/parser/parse_type.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

#include "pgcpp/catalog/catalog.hpp"
#include "pgcpp/catalog/pg_type.hpp"
#include "pgcpp/catalog/syscache.hpp"
#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/parser/parse_node.hpp"
#include "pgcpp/parser/parsenodes.hpp"
#include "pgcpp/types/datum.hpp"

namespace pgcpp::parser {

using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_type;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::GetSysCache;
using pgcpp::catalog::kInvalidOid;
using pgcpp::catalog::Oid;
using pgcpp::nodes::Node;
using pgcpp::nodes::NodeTag;
using pgcpp::nodes::nodeTag;
using pgcpp::nodes::Value;

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
    {"bool", pgcpp::types::kBoolOid, 1, true},
    {"boolean", pgcpp::types::kBoolOid, 1, true},
    {"int2", pgcpp::types::kInt2Oid, 2, true},
    {"smallint", pgcpp::types::kInt2Oid, 2, true},
    {"int4", pgcpp::types::kInt4Oid, 4, true},
    {"integer", pgcpp::types::kInt4Oid, 4, true},
    {"int", pgcpp::types::kInt4Oid, 4, true},
    {"int8", pgcpp::types::kInt8Oid, 8, true},
    {"bigint", pgcpp::types::kInt8Oid, 8, true},
    {"float4", pgcpp::types::kFloat4Oid, 4, true},
    {"real", pgcpp::types::kFloat4Oid, 4, true},
    {"float8", pgcpp::types::kFloat8Oid, 8, true},
    {"double", pgcpp::types::kFloat8Oid, 8, true},
    {"double precision", pgcpp::types::kFloat8Oid, 8, true},
    {"text", pgcpp::types::kTextOid, -1, false},
    {"varchar", pgcpp::types::kVarcharOid, -1, false},
    {"char", pgcpp::types::kTextOid, -1, false},
    {"bpchar", pgcpp::types::kTextOid, -1, false},
    {"date", pgcpp::types::kDateOid, 4, true},
    {"timestamp", pgcpp::types::kTimestampOid, 8, true},
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
    return type_oid == pgcpp::types::kInt2Oid || type_oid == pgcpp::types::kInt4Oid ||
           type_oid == pgcpp::types::kInt8Oid || type_oid == pgcpp::types::kFloat4Oid ||
           type_oid == pgcpp::types::kFloat8Oid;
}

// type_is_string — is this a string type (text, varchar, unknown)?
bool type_is_string(Oid type_oid) {
    return type_oid == pgcpp::types::kTextOid || type_oid == pgcpp::types::kVarcharOid ||
           type_oid == kUnknownOid;
}

// type_is_datetime — is this a date/time type?
bool type_is_datetime(Oid type_oid) {
    return type_oid == pgcpp::types::kDateOid || type_oid == pgcpp::types::kTimestampOid;
}

// ---------------------------------------------------------------------------
// LookupTypeName — array suffix + typmod-aware type resolution.
// ---------------------------------------------------------------------------

namespace {

// VARHDRSZ — PostgreSQL's varlena header size, used when encoding typmods
// for varlena types like varchar/bpchar. Matches VARHDRSZ in postgres.h.
constexpr int32_t kVarHdrSz = 4;

// Built-in base-type → array-type OID mapping. PostgreSQL's pg_type
// normally provides this via FormData_pg_type.typarray, but our
// BootstrapCatalog does not populate pg_type, so we hardcode the
// standard OIDs for the types ClickBench may encounter.
struct ArrayTypeEntry {
    Oid base_oid;
    Oid array_oid;
};

const ArrayTypeEntry kArrayTypes[] = {
    {pgcpp::types::kBoolOid, 1000},       // _bool
    {pgcpp::types::kInt2Oid, 1005},       // _int2
    {pgcpp::types::kInt4Oid, 1007},       // _int4
    {pgcpp::types::kInt8Oid, 1016},       // _int8
    {pgcpp::types::kFloat4Oid, 1021},     // _float4
    {pgcpp::types::kFloat8Oid, 1022},     // _float8
    {pgcpp::types::kTextOid, 1009},       // _text
    {pgcpp::types::kVarcharOid, 1015},    // _varchar
    {pgcpp::types::kDateOid, 1182},       // _date
    {pgcpp::types::kTimestampOid, 1115},  // _timestamp
    {kUnknownOid, 705},                   // _unknown (same OID)
};
constexpr int kArrayTypesCount = sizeof(kArrayTypes) / sizeof(kArrayTypes[0]);

Oid LookupArrayTypeOid(Oid base_oid) {
    for (int i = 0; i < kArrayTypesCount; ++i) {
        if (kArrayTypes[i].base_oid == base_oid)
            return kArrayTypes[i].array_oid;
    }
    return kInvalidOid;
}

// Extract the type name string from a TypeName's `names` list (the last
// component of a qualified name, e.g., "mytype" from "public.mytype").
std::string ExtractLastTypeName(const TypeName* tn) {
    if (tn == nullptr || tn->names.empty())
        return "";
    const Node* last = tn->names.back();
    if (last == nullptr || last->GetTag() != NodeTag::kString)
        return "";
    return static_cast<const Value*>(last)->GetString();
}

// Try to extract an integer typmod from tn->typmods.
// Currently handles the simple case: a single integer Value, used directly
// (with VARHDRSZ added for varlena string types). Returns -1 if no typmod
// can be extracted or the type does not accept a typmod.
int32_t ComputeTypmod(const TypeName* tn, Oid base_oid) {
    if (tn == nullptr || tn->typmods.empty())
        return -1;

    // Only handle a single integer typmod (e.g., varchar(10), char(5)).
    if (tn->typmods.size() != 1)
        return -1;

    const Node* mod_node = tn->typmods[0];
    if (mod_node == nullptr)
        return -1;

    // Integer Value? (scanner emits an integer constant as typmod)
    if (mod_node->GetTag() == NodeTag::kInteger) {
        int64_t ival = static_cast<const Value*>(mod_node)->GetInteger();
        if (ival < 0)
            return -1;

        // Varchar/bpchar/char encode the typmod as VARHDRSZ + max_length
        // to match PostgreSQL's varchar_typmod_in().
        if (base_oid == pgcpp::types::kVarcharOid || base_oid == pgcpp::types::kTextOid) {
            return static_cast<int32_t>(ival + kVarHdrSz);
        }
        // Other types: pass the typmod through verbatim.
        return static_cast<int32_t>(ival);
    }

    // A_Expr / other typmod forms are out of scope for this task.
    return -1;
}

}  // namespace

// LookupTypeName — see header for contract.
Oid LookupTypeName(ParseState* pstate, TypeName* tn, int32_t* typmod) {
    (void)pstate;  // No ParseState usage yet; namespace lookups are deferred.

    if (typmod != nullptr)
        *typmod = -1;

    if (tn == nullptr)
        return kInvalidOid;

    // 1. Resolve the base type name (last component of the qualified name).
    std::string name = ExtractLastTypeName(tn);
    if (name.empty())
        return kInvalidOid;

    Oid base_oid = typenameTypeId(name);
    if (base_oid == kInvalidOid)
        return kInvalidOid;

    // 2. Compute typmod (only applies to the base type — array types
    //    carry their element type's typmod implicitly).
    int32_t computed_typmod = ComputeTypmod(tn, base_oid);
    if (typmod != nullptr)
        *typmod = computed_typmod;

    // 3. If an array suffix is present, look up the corresponding array
    //    type OID. The typmod computed above stays attached to the element
    //    type (PostgreSQL semantics).
    Oid result_oid = base_oid;
    if (!tn->array_bounds.empty()) {
        Oid array_oid = LookupArrayTypeOid(base_oid);
        if (array_oid != kInvalidOid)
            result_oid = array_oid;
        // If we have no array type registered for this base type, leave
        // result_oid as the base type — callers can detect the mismatch.
    }

    tn->type_oid = result_oid;
    return result_oid;
}

// typenameTypeId (strict overload) — see header for contract.
Oid typenameTypeId(ParseState* pstate, TypeName* tn, int32_t* typmod) {
    Oid oid = LookupTypeName(pstate, tn, typmod);
    if (oid == kInvalidOid) {
        std::string name = ExtractLastTypeName(tn);
        ereport(pgcpp::error::LogLevel::kError, "type \"" + name + "\" does not exist");
    }
    return oid;
}

}  // namespace pgcpp::parser

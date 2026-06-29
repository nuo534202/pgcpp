// parse_type.h — Type lookup helpers for parse analysis.
//
// Converted from PostgreSQL 15's src/include/parser/parse_type.h.
#pragma once

#include <cstdint>
#include <string>

#include "catalog/catalog.hpp"
#include "catalog/pg_type.hpp"

namespace pgcpp::parser {

class ParseState;
class TypeName;

using pgcpp::catalog::Oid;

// typenameTypeId — resolve a type name string to its OID.
// Returns InvalidOid if the type is not found.
Oid typenameTypeId(const std::string& typename_name);

// LookupTypeName — resolve a TypeName* (with optional array suffix and
// type modifiers) to a type OID.
//
// On success:
//   * Sets *typmod (if non-null) — for types that accept a typmod
//     (e.g., varchar(10)), this is the encoded typmod value; otherwise -1.
//   * Updates tn->type_oid to the resolved OID (when tn is non-null).
// Returns the resolved OID, or kInvalidOid if the type does not exist
// (no error is raised — the caller decides what to do).
//
// Array suffix handling: when tn->array_bounds is non-empty, the base
// type is resolved first, then the corresponding array type OID is
// returned (e.g., int4[] -> _int4).
Oid LookupTypeName(ParseState* pstate, TypeName* tn, int32_t* typmod);

// typenameTypeId (strict overload) — like LookupTypeName but ereports
// ERROR when the type does not resolve. Mirrors PostgreSQL's
// typenameTypeId(pstate, typeName, *typmod) entry point.
Oid typenameTypeId(ParseState* pstate, TypeName* tn, int32_t* typmod);

// typeTypeId — get the OID from a pg_type row.
Oid typeTypeId(const pgcpp::catalog::FormData_pg_type* type_tuple);

// typeLen — get the typlen from a pg_type row.
int16_t typeLen(const pgcpp::catalog::FormData_pg_type* type_tuple);

// typeByVal — get the typbyval from a pg_type row.
bool typeByVal(const pgcpp::catalog::FormData_pg_type* type_tuple);

// get_typname — look up a type name by OID.
std::string get_typname(Oid type_oid);

// get_typlen — get the typlen for a type OID.
int16_t get_typlen(Oid type_oid);

// get_typbyval — get the typbyval for a type OID.
bool get_typbyval(Oid type_oid);

// Type category predicates.
bool type_is_numeric(Oid type_oid);
bool type_is_string(Oid type_oid);
bool type_is_datetime(Oid type_oid);

}  // namespace pgcpp::parser

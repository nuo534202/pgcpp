// parse_type.h — Type lookup helpers for parse analysis.
//
// Converted from PostgreSQL 15's src/include/parser/parse_type.h.
#pragma once

#include <cstdint>
#include <string>

#include "mytoydb/catalog/catalog.hpp"
#include "mytoydb/catalog/pg_type.hpp"

namespace mytoydb::parser {

using mytoydb::catalog::Oid;

// typenameTypeId — resolve a type name to its OID.
// Returns InvalidOid if the type is not found.
Oid typenameTypeId(const std::string& typename_name);

// typeTypeId — get the OID from a pg_type row.
Oid typeTypeId(const mytoydb::catalog::FormData_pg_type* type_tuple);

// typeLen — get the typlen from a pg_type row.
int16_t typeLen(const mytoydb::catalog::FormData_pg_type* type_tuple);

// typeByVal — get the typbyval from a pg_type row.
bool typeByVal(const mytoydb::catalog::FormData_pg_type* type_tuple);

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

}  // namespace mytoydb::parser

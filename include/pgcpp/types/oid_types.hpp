#pragma once

#include <cstdint>

#include "pgcpp/types/datum.hpp"

namespace pgcpp::types {

// PostgreSQL OID family — uint32 underlying type with name<->OID resolution.
// For pgcpp, we keep the storage as uint32 (Datum by-value) and provide
// in/out that follow PG conventions: input may accept either a numeric OID
// or a name (resolved through a stub name->oid table), output emits the
// registered name when known, otherwise the numeric OID.

// oid — generic OID type (OID 26).
Datum oid_in(const char* str);
char* oid_out(Datum value);
int oid_cmp(Datum a, Datum b);
Datum oid_eq(Datum a, Datum b);
Datum oid_lt(Datum a, Datum b);
Datum oid_gt(Datum a, Datum b);

// regproc — pg_proc row name or OID (OID 24).
Datum regproc_in(const char* str);
char* regproc_out(Datum value);

// regprocedure — pg_proc name with argument types (OID 2202).
Datum regprocedure_in(const char* str);
char* regprocedure_out(Datum value);

// regoper — pg_operator name or OID (OID 2203).
Datum regoper_in(const char* str);
char* regoper_out(Datum value);

// regoperator — pg_operator name with argument types (OID 2204).
Datum regoperator_in(const char* str);
char* regoperator_out(Datum value);

// regclass — pg_class row name or OID (OID 2205).
Datum regclass_in(const char* str);
char* regclass_out(Datum value);

// regtype — pg_type row name or OID (OID 2206).
Datum regtype_in(const char* str);
char* regtype_out(Datum value);

// regnamespace — pg_namespace name or OID (OID 4089).
Datum regnamespace_in(const char* str);
char* regnamespace_out(Datum value);

// regrole — pg_authid name or OID (OID 4096).
Datum regrole_in(const char* str);
char* regrole_out(Datum value);

// Comparison operators shared by all reg* types.
Datum oidfamily_eq(Datum a, Datum b);
Datum oidfamily_ne(Datum a, Datum b);
Datum oidfamily_lt(Datum a, Datum b);
Datum oidfamily_le(Datum a, Datum b);
Datum oidfamily_gt(Datum a, Datum b);
Datum oidfamily_ge(Datum a, Datum b);
int oidfamily_cmp(Datum a, Datum b);

// --- registry helpers ---
// Register a name<->oid mapping for one of the reg* catalogs. Used by the
// reg*_out functions to render names instead of bare OID numbers. Returns
// the number of entries registered.
enum class RegCatalog : int {
    kProc = 0,
    kOper = 1,
    kClass = 2,
    kType = 3,
    kNamespace = 4,
    kRole = 5,
};

// Register a (oid, name) pair in the specified catalog.
void RegisterRegName(RegCatalog cat, uint32_t oid, const char* name);
// Look up a name for the given OID in the specified catalog. Returns nullptr
// when no entry was registered.
const char* LookupRegName(RegCatalog cat, uint32_t oid);
// Look up the OID for a given name (case-sensitive). Returns 0 if not found.
uint32_t LookupRegOid(RegCatalog cat, const char* name);
// Reset all registered entries (used by tests).
void ResetRegCatalogs();

}  // namespace pgcpp::types

// lsyscache.h — low-level syscache convenience lookups.
//
// Converted from PostgreSQL 15's src/include/utils/lsyscache.h and
// src/backend/utils/cache/lsyscache.c.
//
// These are thin wrappers over the global Catalog (which in PG terms is the
// SysCache). All functions return InvalidOid / nullptr / false when the
// requested row is missing — they do NOT ereport(ERROR), matching the
// PostgreSQL lsyscache contract. String results are palloc'd char* in the
// current memory context; callers release them via the memory context or
// pfree.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pgcpp/catalog/catalog.hpp"

namespace pgcpp::catalog {

// Sentinel for "attribute not found", mirroring PostgreSQL's InvalidAttrNumber.
constexpr int16_t kInvalidAttrNumber = 0;

// --- pg_operator lookups ---

// get_opcode — return the implementation function OID (oprcode) of an
// operator. InvalidOid if the operator does not exist or is a shell.
Oid get_opcode(Oid opoid);

// get_op — return the full pg_operator row, or nullptr if not found.
const FormData_pg_operator* get_op(Oid opoid);

// get_opname — return a palloc'd copy of the operator name.
// Returns nullptr if the operator does not exist.
char* get_opname(Oid opoid);

// get_commutator — return the commutator operator OID (oprcom).
Oid get_commutator(Oid opoid);

// get_negator — return the negator operator OID (oprnegate).
Oid get_negator(Oid opoid);

// op_mergejoinable — return true if the operator can be used in a merge join.
// When non-null, *left and *right are filled with the operator's operand
// types (oprleft / oprright).
bool op_mergejoinable(Oid opoid, Oid* left, Oid* right);

// op_strict — return true if the operator's implementation function is
// strict (proisstrict). Returns false if the operator or function is missing.
bool op_strict(Oid opoid);

// --- pg_proc lookups ---

// get_func_rettype — return the result type OID (prorettype).
Oid get_func_rettype(Oid funcoid);

// get_func_name — return a palloc'd copy of the function name (proname).
// Returns nullptr if not found.
char* get_func_name(Oid funcoid);

// get_func_prokind — return the prokind char ('f', 'a', 'w', 'p').
// Returns '\0' if the function does not exist.
char get_func_prokind(Oid funcoid);

// get_func_nargs — return the number of arguments (pronargs).
// Returns -1 if the function does not exist.
int get_func_nargs(Oid funcoid);

// --- pg_type lookups ---

// get_type_name — return a palloc'd copy of the type name (typname).
// Returns nullptr if not found.
char* get_type_name(Oid typoid);

// get_typlen — return the type's length (-1 for varlena). Returns 0 if
// the type does not exist.
int16_t get_typlen(Oid typoid);

// get_typbyval — return true if the type is pass-by-value.
bool get_typbyval(Oid typoid);

// get_typalign — return the alignment char ('c', 's', 'i', 'd').
// Returns '\0' if the type does not exist.
char get_typalign(Oid typoid);

// get_typstorage — return the storage strategy char ('p', 'e', 'x', 'm').
// Returns '\0' if the type does not exist.
char get_typstorage(Oid typoid);

// get_typcategory — return the type category char ('A'..'X').
// Returns '\0' if the type does not exist.
char get_typcategory(Oid typoid);

// get_typisdefined — return true if the type is fully defined.
bool get_typisdefined(Oid typoid);

// get_typelem — return the array element type OID (typelem).
Oid get_typelem(Oid typoid);

// --- pg_attribute lookups ---

// get_att — return the pg_attribute row for (relid, attname), or nullptr.
const FormData_pg_attribute* get_att(Oid relid, const char* attname);

// get_attname — return a palloc'd copy of the column name for attnum.
// Returns nullptr if the attribute does not exist. attnum is 1-based.
char* get_attname(Oid relid, int16_t attnum);

// get_atttype — return the column's type OID (atttypid).
Oid get_atttype(Oid relid, int16_t attnum);

// get_attnum — return the column number for a given column name.
// Returns InvalidAttrNumber (0) if not found.
int16_t get_attnum(Oid relid, const char* attname);

// get_attnotnull — return true if the column has a NOT NULL constraint.
bool get_attnotnull(Oid relid, int16_t attnum);

// --- pg_class lookups ---

// get_rel_name — return a palloc'd copy of the relation name (relname).
// Returns nullptr if not found.
char* get_rel_name(Oid relid);

// get_rel_relkind — return the relkind char ('r', 'i', 'S', 'v', ...).
// Returns '\0' if the relation does not exist.
char get_rel_relkind(Oid relid);

// get_rel_persistence — return the relpersistence char ('p', 't', 'u').
// Returns '\0' if the relation does not exist.
char get_rel_persistence(Oid relid);

// get_rel_namespace — return the namespace OID (relnamespace).
Oid get_rel_namespace(Oid relid);

// --- Convenience predicates ---

// type_is_rowtype — return true if the type is a composite/row type
// (typtype == 'c'). Returns false if the type does not exist.
bool type_is_rowtype(Oid typoid);

// type_is_enum — return true if the type is an enum (typtype == 'e').
// pgcpp does not yet model enums, so this always returns false.
bool type_is_enum(Oid typoid);

}  // namespace pgcpp::catalog

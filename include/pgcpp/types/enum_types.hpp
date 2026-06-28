#pragma once

#include <cstdint>

#include "pgcpp/types/datum.hpp"

namespace pgcpp::types {

// ---------------------------------------------------------------------------
// Enum types (PostgreSQL utils/adt/enum.c).
//
// PostgreSQL stores enum labels as rows in pg_enum. For pgcpp we provide an
// in-memory registry of (enumtypoid, label)<->sortorder mappings plus the
// enum_in/enum_out/enum_cmp family of functions.
//
// enum_cmp uses the registered sortorder, falling back to label comparison
// when the registration is missing.
// ---------------------------------------------------------------------------

// Register an enum label under a given enum type OID. The first registration
// assigns sortorder 1, the next 2, etc. (per type). Re-registering the same
// label is a no-op.
void EnumRegisterLabel(uint32_t enum_type_oid, const char* label);

// Look up the sortorder of a label. Returns -1 when unknown.
int32_t EnumLookupLabel(uint32_t enum_type_oid, const char* label);

// Look up the label for a sortorder. Returns nullptr when unknown.
const char* EnumLookupSortorder(uint32_t enum_type_oid, int32_t sortorder);

// Reset the entire enum registry (used by tests).
void EnumResetRegistry();

// enum_in — parse a label into a sortorder-backed Datum.
Datum enum_in(const char* str, uint32_t enum_type_oid);

// enum_out — format a Datum as its label.
char* enum_out(Datum value, uint32_t enum_type_oid);

// enum_cmp — compare by sortorder.
int enum_cmp(Datum a, Datum b);
Datum enum_eq(Datum a, Datum b);
Datum enum_lt(Datum a, Datum b);

}  // namespace pgcpp::types

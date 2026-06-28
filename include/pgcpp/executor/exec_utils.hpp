// exec_utils.h — Shared executor utility helpers.
//
// Helpers used by multiple executor node implementations:
//   BuildTupleDescFromTargetList — construct a TupleDesc from a plan's
//   target list, determining each output column's type from the expression.
#pragma once

#include <cstdint>
#include <string>

#include "mytoydb/access/rel.hpp"
#include "mytoydb/catalog/catalog.hpp"
#include "mytoydb/common/containers/node.hpp"
#include "mytoydb/executor/tupletable.hpp"
#include "mytoydb/parser/primnodes.hpp"
#include "mytoydb/types/datum.hpp"

namespace mytoydb::executor {

// Build a TupleDesc from a target list.
// Each TargetEntry contributes one attribute. The attribute type is
// determined by examining the expression:
//   Var       → vartype
//   Const     → consttype
//   Aggref    → aggtype
//   OpExpr    → opresulttype
//   default   → int4
mytoydb::access::TupleDesc BuildTupleDescFromTargetList(
    const std::vector<mytoydb::parser::TargetEntry*>& targetlist);

// Fill in attlen/attbyval/attalign for a known type OID.
// Defaults to int4 (len=4, byval=true, align=int) for unknown types.
void FillTypeAttrs(mytoydb::catalog::Oid typid, int16_t* attlen, bool* attbyval,
                   mytoydb::catalog::AttAlign* attalign);

// Compare two Datum values of the given type.
// Returns -1 if a < b, 0 if a == b, 1 if a > b. NULLs sort before non-NULLs.
int CompareDatumValues(mytoydb::types::Datum a, bool a_null, mytoydb::types::Datum b, bool b_null,
                       mytoydb::catalog::Oid typid);

// Compare two TupleTableSlots on the given 1-based attribute numbers.
// Returns -1 / 0 / 1. Uses the slot's tuple descriptor to determine types.
// Both slots must share the same tuple descriptor (same column types).
int CompareTuplesOnAttrs(const TupleTableSlot* a, const TupleTableSlot* b,
                         const std::vector<int>& attnos);

}  // namespace mytoydb::executor

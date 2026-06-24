// exec_utils.h — Shared executor utility helpers.
//
// Helpers used by multiple executor node implementations:
//   BuildTupleDescFromTargetList — construct a TupleDesc from a plan's
//   target list, determining each output column's type from the expression.
#pragma once

#include <cstdint>
#include <string>

#include "mytoydb/access/rel.h"
#include "mytoydb/catalog/catalog.h"
#include "mytoydb/common/containers/node.h"
#include "mytoydb/parser/primnodes.h"
#include "mytoydb/types/datum.h"

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

}  // namespace mytoydb::executor

// makefuncs.h — C++ version of PostgreSQL's makefuncs.c (P0 subset).
//
// Converted from PostgreSQL 15's src/include/utils/makefuncs.h and
// src/backend/nodes/makefuncs.c. Provides factory functions that construct
// transformed expression nodes (Var/Const/TargetEntry/OpExpr/FuncExpr/
// RelabelType/BoolExpr) in the current memory context via makePallocNode.
//
// Note: mytoydb::parser already declares makeVar/makeConst/makeNullConst with
// a different (smaller) signature in primnodes.hpp. The overloads here live
// in namespace mytoydb::nodes and add the full-parameter variants needed by
// the planner. Callers that want the parser-layer convenience constructors
// should use mytoydb::parser::makeVar etc. directly.

#pragma once

#include <string>
#include <vector>

#include "mytoydb/catalog/catalog.hpp"         // Oid
#include "mytoydb/common/containers/node.hpp"  // makePallocNode
#include "mytoydb/parser/primnodes.hpp"        // Var/Const/OpExpr/...

namespace mytoydb::nodes {

using mytoydb::catalog::Oid;
using mytoydb::parser::BoolExpr;
using mytoydb::parser::BoolExprType;
using mytoydb::parser::CoercionForm;
using mytoydb::parser::Const;
using mytoydb::parser::FuncExpr;
using mytoydb::parser::Node;
using mytoydb::parser::OpExpr;
using mytoydb::parser::RelabelType;
using mytoydb::parser::TargetEntry;
using mytoydb::parser::Var;
using mytoydb::types::Datum;

// AttrNumber is int32 in PG; we use int for the C++ conversion (per the
// project rule that Oid/int32 fields become int).
using AttrNumber = int;

// --- Var -------------------------------------------------------------------

// Full-parameter makeVar (PG makeVar extended form). Sets varnosyn/varattnosyn
// explicitly.
Var* makeVar(int varno, int varattno, Oid vartype, int vartypmod, Oid varcollid, int varlevelsup,
             int varnosyn, int varattnosyn, int location);

// Convenience makeVar: varlevelsup=0, varnosyn=varno, varattnosyn=varattno,
// vartypmod=-1, varcollid=InvalidOid, location=-1.
Var* makeVar(int varno, int varattno, Oid vartype);

// --- Const -----------------------------------------------------------------

Const* makeConst(Oid consttype, int consttypmod, Oid constcollid, int constlen, Datum constvalue,
                 bool constisnull, bool constbyval, int location);

// Construct a NULL Const of the given type (PG makeNullConst). constlen /
// constbyval default to 0/false; callers that need them should follow up by
// setting the fields directly.
Const* makeNullConst(Oid consttype, int consttypmod, Oid constcollid, int location);

// --- TargetEntry -----------------------------------------------------------

TargetEntry* makeTargetEntry(Node* expr, AttrNumber resno, const std::string& resname,
                             bool resjunk);

// --- OpExpr ----------------------------------------------------------------

// Full-parameter makeOpExpr.
OpExpr* makeOpExpr(Oid opno, Oid opresulttype, bool opretset, Oid opcollid, Oid inputcollid,
                   std::vector<Node*> args, int location);

// Convenience 2-operand makeOpExpr: opretset=false, opcollid=InvalidOid,
// inputcollid=InvalidOid, location=-1.
OpExpr* makeOpExpr(Oid opno, Oid opresulttype, Node* left, Node* right);

// --- FuncExpr --------------------------------------------------------------

FuncExpr* makeFuncExpr(Oid funcid, Oid funcresulttype, std::vector<Node*> args, Oid funccollid,
                       Oid inputcollid, bool funcretset, CoercionForm funcformat, int location);

// --- RelabelType -----------------------------------------------------------

RelabelType* makeRelabelType(Node* arg, Oid resulttype, int resulttypmod, Oid resultcollid,
                             CoercionForm relabelformat, int location);

// --- BoolExpr --------------------------------------------------------------

BoolExpr* makeBoolExpr(BoolExprType boolop, std::vector<Node*> args, int location);

}  // namespace mytoydb::nodes

// makefuncs.cpp — factory functions for transformed expression nodes.
//
// See makefuncs.hpp. All nodes are allocated via makePallocNode<T>() in the
// current memory context, then their public fields are filled in. Default
// field values come from the struct definitions in primnodes.hpp (e.g.
// Var::vartypmod defaults to -1).

#include "pgcpp/common/containers/makefuncs.hpp"

#include <utility>

#include "pgcpp/catalog/catalog.hpp"  // kInvalidOid

namespace pgcpp::nodes {

constexpr Oid kInvalidOid = pgcpp::catalog::kInvalidOid;

// --- Var -------------------------------------------------------------------

Var* makeVar(int varno, int varattno, Oid vartype, int vartypmod, Oid varcollid, int varlevelsup,
             int varnosyn, int varattnosyn, int location) {
    auto* var = makePallocNode<Var>();
    var->varno = varno;
    var->varattno = varattno;
    var->vartype = vartype;
    var->vartypmod = vartypmod;
    var->varcollid = varcollid;
    var->varlevelsup = varlevelsup;
    var->varnosyn = varnosyn;
    var->varattnosyn = varattnosyn;
    var->location = location;
    return var;
}

Var* makeVar(int varno, int varattno, Oid vartype) {
    return makeVar(varno, varattno, vartype, /*vartypmod=*/-1, /*varcollid=*/kInvalidOid,
                   /*varlevelsup=*/0, /*varnosyn=*/varno, /*varattnosyn=*/varattno,
                   /*location=*/-1);
}

// --- Const -----------------------------------------------------------------

Const* makeConst(Oid consttype, int consttypmod, Oid constcollid, int constlen, Datum constvalue,
                 bool constisnull, bool constbyval, int location) {
    auto* con = makePallocNode<Const>();
    con->consttype = consttype;
    con->consttypmod = consttypmod;
    con->constcollid = constcollid;
    con->constlen = constlen;
    con->constvalue = constisnull ? 0 : constvalue;
    con->constisnull = constisnull;
    con->constbyval = constbyval;
    con->location = location;
    return con;
}

Const* makeNullConst(Oid consttype, int consttypmod, Oid constcollid, int location) {
    return makeConst(consttype, consttypmod, constcollid, /*constlen=*/0, /*constvalue=*/0,
                     /*constisnull=*/true, /*constbyval=*/false, location);
}

// --- TargetEntry -----------------------------------------------------------

TargetEntry* makeTargetEntry(Node* expr, AttrNumber resno, const std::string& resname,
                             bool resjunk) {
    auto* te = makePallocNode<TargetEntry>();
    te->expr = expr;
    te->resno = resno;
    te->resname = resname;
    te->ressortgroupref = 0;
    te->resorigtbl = kInvalidOid;
    te->resorigcol = 0;
    te->resjunk = resjunk;
    return te;
}

// --- OpExpr ----------------------------------------------------------------

OpExpr* makeOpExpr(Oid opno, Oid opresulttype, bool opretset, Oid opcollid, Oid inputcollid,
                   std::vector<Node*> args, int location) {
    auto* op = makePallocNode<OpExpr>();
    op->opno = opno;
    op->opfuncid = kInvalidOid;
    op->opresulttype = opresulttype;
    op->opretset = opretset;
    op->opcollid = opcollid;
    op->inputcollid = inputcollid;
    op->args = std::move(args);
    op->location = location;
    return op;
}

OpExpr* makeOpExpr(Oid opno, Oid opresulttype, Node* left, Node* right) {
    std::vector<Node*> args;
    args.reserve(2);
    args.push_back(left);
    args.push_back(right);
    return makeOpExpr(opno, opresulttype, /*opretset=*/false, /*opcollid=*/kInvalidOid,
                      /*inputcollid=*/kInvalidOid, std::move(args), /*location=*/-1);
}

// --- FuncExpr --------------------------------------------------------------

FuncExpr* makeFuncExpr(Oid funcid, Oid funcresulttype, std::vector<Node*> args, Oid funccollid,
                       Oid inputcollid, bool funcretset, CoercionForm funcformat, int location) {
    auto* fn = makePallocNode<FuncExpr>();
    fn->funcid = funcid;
    fn->funcresulttype = funcresulttype;
    fn->funcretset = funcretset;
    fn->funcvariadic = false;
    fn->funcformat = funcformat;
    fn->funccollid = funccollid;
    fn->inputcollid = inputcollid;
    fn->args = std::move(args);
    fn->location = location;
    return fn;
}

// --- RelabelType -----------------------------------------------------------

RelabelType* makeRelabelType(Node* arg, Oid resulttype, int resulttypmod, Oid resultcollid,
                             CoercionForm relabelformat, int location) {
    auto* r = makePallocNode<RelabelType>();
    r->arg = arg;
    r->resulttype = resulttype;
    r->resulttypmod = resulttypmod;
    r->resultcollid = resultcollid;
    r->relabelformat = relabelformat;
    r->location = location;
    return r;
}

// --- BoolExpr --------------------------------------------------------------

BoolExpr* makeBoolExpr(BoolExprType boolop, std::vector<Node*> args, int location) {
    auto* b = makePallocNode<BoolExpr>();
    b->boolop = boolop;
    b->args = std::move(args);
    b->location = location;
    return b;
}

}  // namespace pgcpp::nodes

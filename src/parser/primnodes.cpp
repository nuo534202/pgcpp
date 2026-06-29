// primnodes.cpp — Clone() and Equals() implementations for transformed
// expression node types (primnodes.h).
//
// Each Clone() uses palloc for the node allocation (matching the
// memory-context model) and deep-copies all Node* and std::vector<Node*>
// fields. Each Equals() compares all fields.

#include "parser/primnodes.hpp"

#include <new>
#include <utility>

#include "common/containers/node.hpp"
#include "common/memory/memory_context.hpp"

namespace pgcpp::parser {
using pgcpp::nodes::makePallocNode;

using pgcpp::nodes::copyObject;
using pgcpp::nodes::equal;
using pgcpp::nodes::Node;

namespace {

std::vector<Node*> CloneVec(const std::vector<Node*>& vec) {
    std::vector<Node*> result;
    result.reserve(vec.size());
    for (Node* n : vec) {
        result.push_back(copyObject(n));
    }
    return result;
}

Node* CloneNode(Node* n) {
    return n == nullptr ? nullptr : copyObject(n);
}

bool EqNode(const Node* a, const Node* b) {
    return equal(a, b);
}

bool EqVec(const std::vector<Node*>& a, const std::vector<Node*>& b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!EqNode(a[i], b[i]))
            return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Var
// ---------------------------------------------------------------------------

Node* Var::Clone() const {
    auto* copy = makePallocNode<Var>(*this);
    return copy;
}

bool Var::Equals(const Node& other) const {
    if (GetTag() != other.GetTag())
        return false;
    const auto& o = static_cast<const Var&>(other);
    return varno == o.varno && varattno == o.varattno && vartype == o.vartype &&
           vartypmod == o.vartypmod && varcollid == o.varcollid && varlevelsup == o.varlevelsup &&
           varnosyn == o.varnosyn && varattnosyn == o.varattnosyn && location == o.location;
}

// ---------------------------------------------------------------------------
// Const
// ---------------------------------------------------------------------------

Node* Const::Clone() const {
    auto* copy = makePallocNode<Const>(*this);
    return copy;
}

bool Const::Equals(const Node& other) const {
    if (GetTag() != other.GetTag())
        return false;
    const auto& o = static_cast<const Const&>(other);
    return consttype == o.consttype && consttypmod == o.consttypmod &&
           constcollid == o.constcollid && constlen == o.constlen && constvalue == o.constvalue &&
           constisnull == o.constisnull && constbyval == o.constbyval && location == o.location;
}

// ---------------------------------------------------------------------------
// Param
// ---------------------------------------------------------------------------

Node* Param::Clone() const {
    auto* copy = makePallocNode<Param>(*this);
    return copy;
}

bool Param::Equals(const Node& other) const {
    if (GetTag() != other.GetTag())
        return false;
    const auto& o = static_cast<const Param&>(other);
    return paramkind == o.paramkind && paramid == o.paramid && paramtype == o.paramtype &&
           paramtypmod == o.paramtypmod && paramcollid == o.paramcollid && location == o.location;
}

// ---------------------------------------------------------------------------
// OpExpr
// ---------------------------------------------------------------------------

Node* OpExpr::Clone() const {
    auto* copy = makePallocNode<OpExpr>(*this);
    copy->args = CloneVec(args);
    return copy;
}

bool OpExpr::Equals(const Node& other) const {
    if (GetTag() != other.GetTag())
        return false;
    const auto& o = static_cast<const OpExpr&>(other);
    return opno == o.opno && opfuncid == o.opfuncid && opresulttype == o.opresulttype &&
           opretset == o.opretset && opcollid == o.opcollid && inputcollid == o.inputcollid &&
           EqVec(args, o.args) && location == o.location;
}

// ---------------------------------------------------------------------------
// FuncExpr
// ---------------------------------------------------------------------------

Node* FuncExpr::Clone() const {
    auto* copy = makePallocNode<FuncExpr>(*this);
    copy->args = CloneVec(args);
    return copy;
}

bool FuncExpr::Equals(const Node& other) const {
    if (GetTag() != other.GetTag())
        return false;
    const auto& o = static_cast<const FuncExpr&>(other);
    return funcid == o.funcid && funcresulttype == o.funcresulttype && funcretset == o.funcretset &&
           funcvariadic == o.funcvariadic && funcformat == o.funcformat &&
           funccollid == o.funccollid && inputcollid == o.inputcollid && EqVec(args, o.args) &&
           location == o.location;
}

// ---------------------------------------------------------------------------
// Aggref
// ---------------------------------------------------------------------------

Node* Aggref::Clone() const {
    auto* copy = makePallocNode<Aggref>(*this);
    copy->aggargtypes = CloneVec(aggargtypes);
    copy->aggdirectargs = CloneVec(aggdirectargs);
    copy->args = CloneVec(args);
    copy->aggorder = CloneVec(aggorder);
    copy->aggdistinct = CloneVec(aggdistinct);
    copy->aggfilter = CloneNode(aggfilter);
    return copy;
}

bool Aggref::Equals(const Node& other) const {
    if (GetTag() != other.GetTag())
        return false;
    const auto& o = static_cast<const Aggref&>(other);
    return aggfnoid == o.aggfnoid && aggtype == o.aggtype && aggcollid == o.aggcollid &&
           inputcollid == o.inputcollid && aggtranstype == o.aggtranstype &&
           EqVec(aggargtypes, o.aggargtypes) && EqVec(aggdirectargs, o.aggdirectargs) &&
           EqVec(args, o.args) && EqVec(aggorder, o.aggorder) &&
           EqVec(aggdistinct, o.aggdistinct) && EqNode(aggfilter, o.aggfilter) &&
           aggstar == o.aggstar && aggvariadic == o.aggvariadic && aggkind == o.aggkind &&
           agglevelsup == o.agglevelsup && aggsplit == o.aggsplit && aggno == o.aggno &&
           aggtransno == o.aggtransno && location == o.location;
}

// ---------------------------------------------------------------------------
// BoolExpr
// ---------------------------------------------------------------------------

Node* BoolExpr::Clone() const {
    auto* copy = makePallocNode<BoolExpr>(*this);
    copy->args = CloneVec(args);
    return copy;
}

bool BoolExpr::Equals(const Node& other) const {
    if (GetTag() != other.GetTag())
        return false;
    const auto& o = static_cast<const BoolExpr&>(other);
    return boolop == o.boolop && EqVec(args, o.args) && location == o.location;
}

// ---------------------------------------------------------------------------
// NullTest
// ---------------------------------------------------------------------------

Node* NullTest::Clone() const {
    auto* copy = makePallocNode<NullTest>(*this);
    copy->arg = CloneNode(arg);
    return copy;
}

bool NullTest::Equals(const Node& other) const {
    if (GetTag() != other.GetTag())
        return false;
    const auto& o = static_cast<const NullTest&>(other);
    return EqNode(arg, o.arg) && nulltesttype == o.nulltesttype && argisrow == o.argisrow &&
           location == o.location;
}

// ---------------------------------------------------------------------------
// BooleanTest
// ---------------------------------------------------------------------------

Node* BooleanTest::Clone() const {
    auto* copy = makePallocNode<BooleanTest>(*this);
    copy->arg = CloneNode(arg);
    return copy;
}

bool BooleanTest::Equals(const Node& other) const {
    if (GetTag() != other.GetTag())
        return false;
    const auto& o = static_cast<const BooleanTest&>(other);
    return EqNode(arg, o.arg) && booltesttype == o.booltesttype && location == o.location;
}

// ---------------------------------------------------------------------------
// CaseExpr
// ---------------------------------------------------------------------------

Node* CaseExpr::Clone() const {
    auto* copy = makePallocNode<CaseExpr>(*this);
    copy->arg = CloneNode(arg);
    copy->args = CloneVec(args);
    copy->defresult = CloneNode(defresult);
    return copy;
}

bool CaseExpr::Equals(const Node& other) const {
    if (GetTag() != other.GetTag())
        return false;
    const auto& o = static_cast<const CaseExpr&>(other);
    return casetype == o.casetype && casecollid == o.casecollid && EqNode(arg, o.arg) &&
           EqVec(args, o.args) && EqNode(defresult, o.defresult) && location == o.location;
}

// ---------------------------------------------------------------------------
// CaseWhen
// ---------------------------------------------------------------------------

Node* CaseWhen::Clone() const {
    auto* copy = makePallocNode<CaseWhen>(*this);
    copy->expr = CloneNode(expr);
    copy->result = CloneNode(result);
    return copy;
}

bool CaseWhen::Equals(const Node& other) const {
    if (GetTag() != other.GetTag())
        return false;
    const auto& o = static_cast<const CaseWhen&>(other);
    return EqNode(expr, o.expr) && EqNode(result, o.result) && location == o.location;
}

// ---------------------------------------------------------------------------
// SubLink
// ---------------------------------------------------------------------------

Node* SubLink::Clone() const {
    auto* copy = makePallocNode<SubLink>(*this);
    copy->testexpr = CloneNode(testexpr);
    copy->opername = CloneVec(opername);
    copy->subselect = CloneNode(subselect);
    return copy;
}

bool SubLink::Equals(const Node& other) const {
    if (GetTag() != other.GetTag())
        return false;
    const auto& o = static_cast<const SubLink&>(other);
    return sublinktype == o.sublinktype && sublinkid == o.sublinkid &&
           EqNode(testexpr, o.testexpr) && EqVec(opername, o.opername) &&
           EqNode(subselect, o.subselect) && location == o.location;
}

// ---------------------------------------------------------------------------
// RelabelType
// ---------------------------------------------------------------------------

Node* RelabelType::Clone() const {
    auto* copy = makePallocNode<RelabelType>(*this);
    copy->arg = CloneNode(arg);
    return copy;
}

bool RelabelType::Equals(const Node& other) const {
    if (GetTag() != other.GetTag())
        return false;
    const auto& o = static_cast<const RelabelType&>(other);
    return EqNode(arg, o.arg) && resulttype == o.resulttype && resulttypmod == o.resulttypmod &&
           resultcollid == o.resultcollid && relabelformat == o.relabelformat &&
           location == o.location;
}

// ---------------------------------------------------------------------------
// CoerceViaIO
// ---------------------------------------------------------------------------

Node* CoerceViaIO::Clone() const {
    auto* copy = makePallocNode<CoerceViaIO>(*this);
    copy->arg = CloneNode(arg);
    return copy;
}

bool CoerceViaIO::Equals(const Node& other) const {
    if (GetTag() != other.GetTag())
        return false;
    const auto& o = static_cast<const CoerceViaIO&>(other);
    return EqNode(arg, o.arg) && resulttype == o.resulttype && resultcollid == o.resultcollid &&
           coerceformat == o.coerceformat && location == o.location;
}

// ---------------------------------------------------------------------------
// ScalarArrayOpExpr
// ---------------------------------------------------------------------------

Node* ScalarArrayOpExpr::Clone() const {
    auto* copy = makePallocNode<ScalarArrayOpExpr>(*this);
    copy->args = CloneVec(args);
    return copy;
}

bool ScalarArrayOpExpr::Equals(const Node& other) const {
    if (GetTag() != other.GetTag())
        return false;
    const auto& o = static_cast<const ScalarArrayOpExpr&>(other);
    return opno == o.opno && opfuncid == o.opfuncid && hashfuncid == o.hashfuncid &&
           negfuncid == o.negfuncid && use_or == o.use_or && inputcollid == o.inputcollid &&
           EqVec(args, o.args) && location == o.location;
}

// ---------------------------------------------------------------------------
// CoerceToDomain
// ---------------------------------------------------------------------------

Node* CoerceToDomain::Clone() const {
    auto* copy = makePallocNode<CoerceToDomain>(*this);
    copy->arg = CloneNode(arg);
    return copy;
}

bool CoerceToDomain::Equals(const Node& other) const {
    if (GetTag() != other.GetTag())
        return false;
    const auto& o = static_cast<const CoerceToDomain&>(other);
    return EqNode(arg, o.arg) && resulttype == o.resulttype && resulttypmod == o.resulttypmod &&
           resultcollid == o.resultcollid && coercionformat == o.coercionformat &&
           location == o.location;
}

// ---------------------------------------------------------------------------
// TargetEntry
// ---------------------------------------------------------------------------

Node* TargetEntry::Clone() const {
    auto* copy = makePallocNode<TargetEntry>(*this);
    copy->expr = CloneNode(expr);
    return copy;
}

bool TargetEntry::Equals(const Node& other) const {
    if (GetTag() != other.GetTag())
        return false;
    const auto& o = static_cast<const TargetEntry&>(other);
    return EqNode(expr, o.expr) && resno == o.resno && resname == o.resname &&
           ressortgroupref == o.ressortgroupref && resorigtbl == o.resorigtbl &&
           resorigcol == o.resorigcol && resjunk == o.resjunk;
}

// ---------------------------------------------------------------------------
// RangeTblRef
// ---------------------------------------------------------------------------

Node* RangeTblRef::Clone() const {
    return makePallocNode<RangeTblRef>(*this);
}

bool RangeTblRef::Equals(const Node& other) const {
    if (GetTag() != other.GetTag())
        return false;
    const auto& o = static_cast<const RangeTblRef&>(other);
    return rtindex == o.rtindex;
}

// ---------------------------------------------------------------------------
// JoinExpr
// ---------------------------------------------------------------------------

Node* JoinExpr::Clone() const {
    auto* copy = makePallocNode<JoinExpr>(*this);
    copy->larg = CloneNode(larg);
    copy->rarg = CloneNode(rarg);
    copy->using_clause = CloneVec(using_clause);
    copy->quals = CloneNode(quals);
    return copy;
}

bool JoinExpr::Equals(const Node& other) const {
    if (GetTag() != other.GetTag())
        return false;
    const auto& o = static_cast<const JoinExpr&>(other);
    return jointype == o.jointype && is_natural == o.is_natural && EqNode(larg, o.larg) &&
           EqNode(rarg, o.rarg) && EqVec(using_clause, o.using_clause) && EqNode(quals, o.quals) &&
           rtindex == o.rtindex;
}

// ---------------------------------------------------------------------------
// FromExpr
// ---------------------------------------------------------------------------

Node* FromExpr::Clone() const {
    auto* copy = makePallocNode<FromExpr>(*this);
    copy->fromlist = CloneVec(fromlist);
    copy->quals = CloneNode(quals);
    return copy;
}

bool FromExpr::Equals(const Node& other) const {
    if (GetTag() != other.GetTag())
        return false;
    const auto& o = static_cast<const FromExpr&>(other);
    return EqVec(fromlist, o.fromlist) && EqNode(quals, o.quals);
}

// ---------------------------------------------------------------------------
// Convenience constructors
// ---------------------------------------------------------------------------

Var* makeVar(int varno, int varattno, Oid vartype, int vartypmod, Oid varcollid, int varlevelsup,
             int location) {
    auto* var = makeNode<Var>();
    var->varno = varno;
    var->varattno = varattno;
    var->vartype = vartype;
    var->vartypmod = vartypmod;
    var->varcollid = varcollid;
    var->varlevelsup = varlevelsup;
    var->varnosyn = varno;
    var->varattnosyn = varattno;
    var->location = location;
    return var;
}

Const* makeConst(Oid consttype, int consttypmod, Oid constcollid, int constlen,
                 pgcpp::types::Datum constvalue, bool constisnull, bool constbyval, int location) {
    auto* con = makeNode<Const>();
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

Const* makeNullConst(Oid consttype, int consttypmod, Oid constcollid) {
    return makeConst(consttype, consttypmod, constcollid, 0, 0, true, false, -1);
}

}  // namespace pgcpp::parser

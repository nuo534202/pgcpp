// primnodes.h — C++ versions of PostgreSQL's transformed expression node types.
//
// Converted from PostgreSQL 15's src/include/nodes/primnodes.h.
// These node types are produced by parse analysis (transformExpr) and represent
// the executable expression tree. All inherit from mytoydb::nodes::Node.
// String fields use std::string, List* fields use std::vector<Node*>,
// and Oid/int32 fields use int.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mytoydb/catalog/catalog.h"
#include "mytoydb/common/containers/node.h"
#include "mytoydb/common/memory/memory_context.h"
#include "mytoydb/parser/parsenodes.h"  // CoercionForm, JoinType, CmdType
#include "mytoydb/types/datum.h"

namespace mytoydb::parser {

using mytoydb::catalog::Oid;
using mytoydb::nodes::Node;

// ---------------------------------------------------------------------------
// Enums for transformed expression nodes (from primnodes.h / nodes.h)
// ---------------------------------------------------------------------------

enum class ParamKind {
    kExtern,
    kExec,
    kSublink,
    kMultiexpr,
};

enum class BoolExprType {
    kAnd,
    kOr,
    kNot,
};

enum class NullTestType {
    kIsNull,
    kIsNotNull,
};

enum class SubLinkType {
    kExists,
    kAll,
    kAny,
    kRowcompare,
    kExpr,
    kMultiexpr,
    kArray,
    kCte,
};

enum class AggSplit {
    kSimple = 0,
    kInitialSerial = 0x06,  // SKIPFINAL | SERIALIZE
    kFinalDeserial = 0x09,  // COMBINE | DESERIALIZE
};

enum class BoolTestType {
    kIsTrue,
    kIsNotTrue,
    kIsFalse,
    kIsNotFalse,
    kIsUnknown,
    kIsNotUnknown,
};

// Special varno values (PostgreSQL INNER_VAR/OUTER_VAR/INDEX_VAR/ROWID_VAR).
constexpr int kInnerVar = -1;
constexpr int kOuterVar = -2;
constexpr int kIndexVar = -3;
constexpr int kRowidVar = -4;

inline bool IsSpecialVarno(int varno) {
    return varno < 0;
}

// ---------------------------------------------------------------------------
// Expr — base class for executable expression nodes.
// ---------------------------------------------------------------------------

class Expr : public Node {
public:
    Node* Clone() const override = 0;
    bool Equals(const Node& other) const override = 0;

protected:
    Expr() = delete;
    explicit Expr(mytoydb::nodes::NodeTag tag) : Node(tag) {}
};

// ---------------------------------------------------------------------------
// Var — variable reference (column reference after analysis).
// ---------------------------------------------------------------------------

class Var : public Expr {
public:
    Var() : Expr(mytoydb::nodes::NodeTag::kVar) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    int varno = 0;        // range table index, or INNER_VAR/OUTER_VAR/etc
    int varattno = 0;     // attribute number (0 = whole-row Var)
    Oid vartype = 0;      // pg_type OID for the type of this var
    int vartypmod = -1;   // pg_attribute typmod value
    Oid varcollid = 0;    // OID of collation, or InvalidOid if none
    int varlevelsup = 0;  // >0 means N levels up in subquery
    int varnosyn = 0;     // syntactic relation index (0 if unknown)
    int varattnosyn = 0;  // syntactic attribute number
    int location = -1;    // token location, or -1 if unknown
};

// ---------------------------------------------------------------------------
// Const — constant value.
// ---------------------------------------------------------------------------

class Const : public Expr {
public:
    Const() : Expr(mytoydb::nodes::NodeTag::kConst) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    Oid consttype = 0;                     // pg_type OID of the constant's datatype
    int consttypmod = -1;                  // typmod value, if any
    Oid constcollid = 0;                   // OID of collation, or InvalidOid if none
    int constlen = 0;                      // typlen of the constant's datatype
    mytoydb::types::Datum constvalue = 0;  // the constant's value
    bool constisnull = false;              // whether the constant is null
    bool constbyval = false;               // whether this datatype is passed by value
    int location = -1;                     // token location, or -1 if unknown
};

// ---------------------------------------------------------------------------
// Param — parameter reference ($1, $2, etc.).
// ---------------------------------------------------------------------------

class Param : public Expr {
public:
    Param() : Expr(mytoydb::nodes::NodeTag::kParam) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    ParamKind paramkind = ParamKind::kExtern;
    int paramid = 0;       // numeric ID for parameter
    Oid paramtype = 0;     // pg_type OID of parameter's datatype
    int paramtypmod = -1;  // typmod value, if known
    Oid paramcollid = 0;   // OID of collation, or InvalidOid if none
    int location = -1;     // token location, or -1 if unknown
};

// ---------------------------------------------------------------------------
// OpExpr — operator expression (a op b).
// ---------------------------------------------------------------------------

class OpExpr : public Expr {
public:
    OpExpr() : Expr(mytoydb::nodes::NodeTag::kOpExpr) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    Oid opno = 0;             // PG_OPERATOR OID of the operator
    Oid opfuncid = 0;         // PG_PROC OID of underlying function
    Oid opresulttype = 0;     // PG_TYPE OID of result value
    bool opretset = false;    // true if operator returns set
    Oid opcollid = 0;         // OID of collation of result
    Oid inputcollid = 0;      // OID of collation that operator should use
    std::vector<Node*> args;  // arguments to the operator (1 or 2)
    int location = -1;        // token location, or -1 if unknown
};

// ---------------------------------------------------------------------------
// FuncExpr — function call expression.
// ---------------------------------------------------------------------------

class FuncExpr : public Expr {
public:
    FuncExpr() : Expr(mytoydb::nodes::NodeTag::kFuncExpr) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    Oid funcid = 0;             // PG_PROC OID of the function
    Oid funcresulttype = 0;     // PG_TYPE OID of result value
    bool funcretset = false;    // true if function returns set
    bool funcvariadic = false;  // true if variadic arguments combined into array
    CoercionForm funcformat = CoercionForm::kImplicit;
    Oid funccollid = 0;       // OID of collation of result
    Oid inputcollid = 0;      // OID of collation that function should use
    std::vector<Node*> args;  // arguments to the function
    int location = -1;        // token location, or -1 if unknown
};

// ---------------------------------------------------------------------------
// Aggref — aggregate function reference (COUNT, SUM, AVG, etc.).
// ---------------------------------------------------------------------------

class Aggref : public Expr {
public:
    Aggref() : Expr(mytoydb::nodes::NodeTag::kAggref) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    Oid aggfnoid = 0;                  // pg_proc Oid of the aggregate
    Oid aggtype = 0;                   // type Oid of result of the aggregate
    Oid aggcollid = 0;                 // OID of collation of result
    Oid inputcollid = 0;               // OID of collation that function should use
    Oid aggtranstype = 0;              // type Oid of aggregate's transition value
    std::vector<Node*> aggargtypes;    // type Oids of direct and aggregated args
    std::vector<Node*> aggdirectargs;  // direct arguments, if ordered-set agg
    std::vector<Node*> args;           // aggregated arguments and sort expressions
    std::vector<Node*> aggorder;       // ORDER BY (list of SortGroupClause)
    std::vector<Node*> aggdistinct;    // DISTINCT (list of SortGroupClause)
    Node* aggfilter = nullptr;         // FILTER expression, if any
    bool aggstar = false;              // true if argument list was really '*'
    bool aggvariadic = false;          // true if variadic arguments combined into array
    char aggkind = 0;                  // aggregate kind (see pg_aggregate.h)
    int agglevelsup = 0;               // > 0 if agg belongs to outer query
    AggSplit aggsplit = AggSplit::kSimple;
    int aggno = -1;      // unique ID within the Agg node
    int aggtransno = 0;  // unique ID of transition state in the Agg
    int location = -1;   // token location, or -1 if unknown
};

// ---------------------------------------------------------------------------
// BoolExpr — boolean expression (AND, OR, NOT).
// ---------------------------------------------------------------------------

class BoolExpr : public Expr {
public:
    BoolExpr() : Expr(mytoydb::nodes::NodeTag::kBoolExpr) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    BoolExprType boolop = BoolExprType::kAnd;
    std::vector<Node*> args;  // arguments to this expression
    int location = -1;        // token location, or -1 if unknown
};

// ---------------------------------------------------------------------------
// NullTest — IS NULL / IS NOT NULL test.
// ---------------------------------------------------------------------------

class NullTest : public Expr {
public:
    NullTest() : Expr(mytoydb::nodes::NodeTag::kNullTest) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    Node* arg = nullptr;  // input expression
    NullTestType nulltesttype = NullTestType::kIsNull;
    bool argisrow = false;  // T to perform field-by-field null checks
    int location = -1;      // token location, or -1 if unknown
};

// ---------------------------------------------------------------------------
// BooleanTest — IS TRUE / IS FALSE / IS UNKNOWN tests.
// ---------------------------------------------------------------------------

class BooleanTest : public Expr {
public:
    BooleanTest() : Expr(mytoydb::nodes::NodeTag::kBooleanTest) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    Node* arg = nullptr;  // input expression
    BoolTestType booltesttype = BoolTestType::kIsTrue;
    int location = -1;  // token location, or -1 if unknown
};

// ---------------------------------------------------------------------------
// CaseExpr — CASE expression.
// ---------------------------------------------------------------------------

class CaseExpr : public Expr {
public:
    CaseExpr() : Expr(mytoydb::nodes::NodeTag::kCaseExpr) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    Oid casetype = 0;           // type of expression result
    Oid casecollid = 0;         // OID of collation, or InvalidOid if none
    Node* arg = nullptr;        // implicit equality comparison argument
    std::vector<Node*> args;    // the arguments (list of CaseWhen)
    Node* defresult = nullptr;  // the default result (ELSE clause)
    int location = -1;          // token location, or -1 if unknown
};

// ---------------------------------------------------------------------------
// CaseWhen — one WHEN clause of a CASE expression.
// ---------------------------------------------------------------------------

class CaseWhen : public Expr {
public:
    CaseWhen() : Expr(mytoydb::nodes::NodeTag::kCaseWhen) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    Node* expr = nullptr;    // condition expression
    Node* result = nullptr;  // substitution result
    int location = -1;       // token location, or -1 if unknown
};

// ---------------------------------------------------------------------------
// SubLink — subquery expression (IN, EXISTS, scalar subquery, etc.).
// ---------------------------------------------------------------------------

class SubLink : public Expr {
public:
    SubLink() : Expr(mytoydb::nodes::NodeTag::kSubLink) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    SubLinkType sublinktype = SubLinkType::kExpr;
    int sublinkid = 0;            // ID (1..n); 0 if not MULTIEXPR
    Node* testexpr = nullptr;     // outer-query test for ALL/ANY/ROWCOMPARE
    std::vector<Node*> opername;  // originally specified operator name
    Node* subselect = nullptr;    // subselect as Query* or raw parsetree
    int location = -1;            // token location, or -1 if unknown
};

// ---------------------------------------------------------------------------
// RelabelType — relabel output type (binary-compatible type coercion).
// ---------------------------------------------------------------------------

class RelabelType : public Expr {
public:
    RelabelType() : Expr(mytoydb::nodes::NodeTag::kRelabelType) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    Node* arg = nullptr;    // input expression
    Oid resulttype = 0;     // output type of coercion expression
    int resulttypmod = -1;  // output typmod (usually -1)
    Oid resultcollid = 0;   // OID of collation, or InvalidOid if none
    CoercionForm relabelformat = CoercionForm::kImplicit;
    int location = -1;  // token location, or -1 if unknown
};

// ---------------------------------------------------------------------------
// CoerceViaIO — coerce via I/O functions (text in/out).
// ---------------------------------------------------------------------------

class CoerceViaIO : public Expr {
public:
    CoerceViaIO() : Expr(mytoydb::nodes::NodeTag::kCoerceViaIO) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    Node* arg = nullptr;   // input expression
    Oid resulttype = 0;    // output type of coercion
    Oid resultcollid = 0;  // OID of collation, or InvalidOid if none
    CoercionForm coerceformat = CoercionForm::kImplicit;
    int location = -1;  // token location, or -1 if unknown
};

// ---------------------------------------------------------------------------
// ScalarArrayOpExpr — scalar OP array (e.g. x IN (1,2,3)).
// ---------------------------------------------------------------------------

class ScalarArrayOpExpr : public Expr {
public:
    ScalarArrayOpExpr() : Expr(mytoydb::nodes::NodeTag::kScalarArrayOpExpr) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    Oid opno = 0;             // PG_OPERATOR OID of the operator
    Oid opfuncid = 0;         // PG_PROC OID of comparison function
    Oid hashfuncid = 0;       // PG_PROC OID of hash func or InvalidOid
    Oid negfuncid = 0;        // PG_PROC OID of negator or InvalidOid
    bool use_or = true;       // true for ANY, false for ALL
    Oid inputcollid = 0;      // OID of collation that operator should use
    std::vector<Node*> args;  // the scalar and array operands
    int location = -1;        // token location, or -1 if unknown
};

// ---------------------------------------------------------------------------
// CoerceToDomain — coerce to domain type.
// ---------------------------------------------------------------------------

class CoerceToDomain : public Expr {
public:
    CoerceToDomain() : Expr(mytoydb::nodes::NodeTag::kCoerceToDomain) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    Node* arg = nullptr;    // input expression
    Oid resulttype = 0;     // domain type ID (result type)
    int resulttypmod = -1;  // output typmod (currently always -1)
    Oid resultcollid = 0;   // OID of collation, or InvalidOid if none
    CoercionForm coercionformat = CoercionForm::kImplicit;
    int location = -1;  // token location, or -1 if unknown
};

// ---------------------------------------------------------------------------
// TargetEntry — one entry in the target list (SELECT output column).
// ---------------------------------------------------------------------------

class TargetEntry : public Expr {
public:
    TargetEntry() : Expr(mytoydb::nodes::NodeTag::kTargetEntry) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    Node* expr = nullptr;     // expression to evaluate
    int resno = 0;            // attribute number
    std::string resname;      // name of the column (could be empty)
    int ressortgroupref = 0;  // nonzero if referenced by a sort/group clause
    Oid resorigtbl = 0;       // OID of column's source table
    int resorigcol = 0;       // column's number in source table
    bool resjunk = false;     // true to eliminate from final target list
};

// ---------------------------------------------------------------------------
// RangeTblRef — reference to a range table entry in the join tree.
// ---------------------------------------------------------------------------

class RangeTblRef : public Node {
public:
    RangeTblRef() : Node(mytoydb::nodes::NodeTag::kRangeTblRef) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    int rtindex = 0;  // index into the range table
};

// ---------------------------------------------------------------------------
// JoinExpr — join expression in the join tree.
// ---------------------------------------------------------------------------

class JoinExpr : public Node {
public:
    JoinExpr() : Node(mytoydb::nodes::NodeTag::kJoinExpr) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    JoinType jointype = JoinType::kInner;
    bool is_natural = false;            // Natural join?
    Node* larg = nullptr;               // left subtree
    Node* rarg = nullptr;               // right subtree
    std::vector<Node*> using_clause;    // USING clause (list of String)
    Alias* join_using_alias = nullptr;  // alias attached to USING clause
    Node* quals = nullptr;              // qualifiers on join, if any
    Alias* alias = nullptr;             // user-written alias clause, if any
    int rtindex = 0;                    // RT index assigned for join, or 0
};

// ---------------------------------------------------------------------------
// FromExpr — FROM clause expression (top of join tree).
// ---------------------------------------------------------------------------

class FromExpr : public Node {
public:
    FromExpr() : Node(mytoydb::nodes::NodeTag::kFromExpr) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    std::vector<Node*> fromlist;  // list of join subtrees
    Node* quals = nullptr;        // qualifiers on join, if any
};

// ---------------------------------------------------------------------------
// makeNode — allocate a default-constructed node in the current memory
// context. Delegates to makePallocNode<T>() so the destructor is registered.
// ---------------------------------------------------------------------------

template<typename T>
T* makeNode() {
    return mytoydb::nodes::makePallocNode<T>();
}

// ---------------------------------------------------------------------------
// makeVar / makeConst / makeOpExpr / makeFuncExpr — convenience constructors.
// ---------------------------------------------------------------------------

Var* makeVar(int varno, int varattno, Oid vartype, int vartypmod, Oid varcollid, int varlevelsup,
             int location);

Const* makeConst(Oid consttype, int consttypmod, Oid constcollid, int constlen,
                 mytoydb::types::Datum constvalue, bool constisnull, bool constbyval, int location);

Const* makeNullConst(Oid consttype, int consttypmod, Oid constcollid);

}  // namespace mytoydb::parser

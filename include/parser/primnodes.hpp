// primnodes.h — C++ versions of PostgreSQL's transformed expression node types.
//
// Converted from PostgreSQL 15's src/include/nodes/primnodes.h.
// These node types are produced by parse analysis (transformExpr) and represent
// the executable expression tree. All inherit from pgcpp::nodes::Node.
// String fields use std::string, List* fields use std::vector<Node*>,
// and Oid/int32 fields use int.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "catalog/catalog.hpp"
#include "common/containers/node.hpp"
#include "common/memory/memory_context.hpp"
#include "parser/parsenodes.hpp"  // CoercionForm, JoinType, CmdType
#include "types/datum.hpp"

namespace pgcpp::parser {

using pgcpp::catalog::Oid;
using pgcpp::nodes::Node;

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
    explicit Expr(pgcpp::nodes::NodeTag tag) : Node(tag) {}
};

// ---------------------------------------------------------------------------
// Var — variable reference (column reference after analysis).
// ---------------------------------------------------------------------------

class Var : public Expr {
public:
    Var() : Expr(pgcpp::nodes::NodeTag::kVar) {}

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
    Const() : Expr(pgcpp::nodes::NodeTag::kConst) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    Oid consttype = 0;                   // pg_type OID of the constant's datatype
    int consttypmod = -1;                // typmod value, if any
    Oid constcollid = 0;                 // OID of collation, or InvalidOid if none
    int constlen = 0;                    // typlen of the constant's datatype
    pgcpp::types::Datum constvalue = 0;  // the constant's value
    bool constisnull = false;            // whether the constant is null
    bool constbyval = false;             // whether this datatype is passed by value
    int location = -1;                   // token location, or -1 if unknown
};

// ---------------------------------------------------------------------------
// Param — parameter reference ($1, $2, etc.).
// ---------------------------------------------------------------------------

class Param : public Expr {
public:
    Param() : Expr(pgcpp::nodes::NodeTag::kParam) {}

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
    OpExpr() : Expr(pgcpp::nodes::NodeTag::kOpExpr) {}

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
    FuncExpr() : Expr(pgcpp::nodes::NodeTag::kFuncExpr) {}

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
    Aggref() : Expr(pgcpp::nodes::NodeTag::kAggref) {}

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
    BoolExpr() : Expr(pgcpp::nodes::NodeTag::kBoolExpr) {}

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
    NullTest() : Expr(pgcpp::nodes::NodeTag::kNullTest) {}

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
    BooleanTest() : Expr(pgcpp::nodes::NodeTag::kBooleanTest) {}

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
    CaseExpr() : Expr(pgcpp::nodes::NodeTag::kCaseExpr) {}

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
    CaseWhen() : Expr(pgcpp::nodes::NodeTag::kCaseWhen) {}

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
    SubLink() : Expr(pgcpp::nodes::NodeTag::kSubLink) {}

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
    RelabelType() : Expr(pgcpp::nodes::NodeTag::kRelabelType) {}

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
    CoerceViaIO() : Expr(pgcpp::nodes::NodeTag::kCoerceViaIO) {}

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
    ScalarArrayOpExpr() : Expr(pgcpp::nodes::NodeTag::kScalarArrayOpExpr) {}

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
    CoerceToDomain() : Expr(pgcpp::nodes::NodeTag::kCoerceToDomain) {}

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
// CoalesceExpr — COALESCE(v1, v2, ...) expression.
// Returns the first non-NULL argument, or NULL if all are NULL.
// ---------------------------------------------------------------------------

class CoalesceExpr : public Expr {
public:
    CoalesceExpr() : Expr(pgcpp::nodes::NodeTag::kCoalesceExpr) {}

    Node* Clone() const override {
        auto* copy = pgcpp::nodes::makePallocNode<CoalesceExpr>(*this);
        copy->args.clear();
        for (Node* n : args) {
            copy->args.push_back(n == nullptr ? nullptr : pgcpp::nodes::copyObject(n));
        }
        return copy;
    }
    bool Equals(const Node& other) const override {
        if (GetTag() != other.GetTag())
            return false;
        const auto& o = static_cast<const CoalesceExpr&>(other);
        if (coalescetype != o.coalescetype || args.size() != o.args.size())
            return false;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (!pgcpp::nodes::equal(args[i], o.args[i]))
                return false;
        }
        return location == o.location;
    }

    Oid coalescetype = 0;     // type of result
    Oid coalescecollid = 0;   // collation of result
    std::vector<Node*> args;  // arguments
    int location = -1;
};

enum class MinMaxOp {
    kIsGreatest,
    kIsLeast,
};

class MinMaxExpr : public Expr {
public:
    MinMaxExpr() : Expr(pgcpp::nodes::NodeTag::kMinMaxExpr) {}

    Node* Clone() const override {
        auto* copy = pgcpp::nodes::makePallocNode<MinMaxExpr>(*this);
        copy->args.clear();
        for (Node* n : args) {
            copy->args.push_back(n == nullptr ? nullptr : pgcpp::nodes::copyObject(n));
        }
        return copy;
    }
    bool Equals(const Node& other) const override {
        if (GetTag() != other.GetTag())
            return false;
        const auto& o = static_cast<const MinMaxExpr&>(other);
        if (minmaxtype != o.minmaxtype || minmaxcollid != o.minmaxcollid ||
            args.size() != o.args.size())
            return false;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (!pgcpp::nodes::equal(args[i], o.args[i]))
                return false;
        }
        return location == o.location;
    }

    MinMaxOp minmaxtype = MinMaxOp::kIsGreatest;
    Oid minmaxcollid = 0;
    Oid inputcollid = 0;
    std::vector<Node*> args;
    int location = -1;
};

class NullIfExpr : public Expr {
public:
    NullIfExpr() : Expr(pgcpp::nodes::NodeTag::kNullIfExpr) {}

    Node* Clone() const override {
        auto* copy = pgcpp::nodes::makePallocNode<NullIfExpr>(*this);
        copy->args.clear();
        for (Node* n : args) {
            copy->args.push_back(n == nullptr ? nullptr : pgcpp::nodes::copyObject(n));
        }
        return copy;
    }
    bool Equals(const Node& other) const override {
        if (GetTag() != other.GetTag())
            return false;
        const auto& o = static_cast<const NullIfExpr&>(other);
        if (opno != o.opno || opresulttype != o.opresulttype || opretset != o.opretset ||
            opcollid != o.opcollid || inputcollid != o.inputcollid || args.size() != o.args.size())
            return false;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (!pgcpp::nodes::equal(args[i], o.args[i]))
                return false;
        }
        return location == o.location;
    }

    Oid opno = 0;          // underlying operator OID
    Oid opresulttype = 0;  // result type
    bool opretset = false;
    Oid opcollid = 0;
    Oid inputcollid = 0;
    std::vector<Node*> args;
    int location = -1;
};

// SQLValueFunctionOp — enum identifying which current_date/time variant
// a SQLValueFunction represents. Mirrors PostgreSQL's SQLValueFunctionOp.
enum class SQLValueFunctionOp {
    kCurrentDate,
    kCurrentTime,
    kCurrentTimeN,
    kCurrentTimestamp,
    kCurrentTimestampN,
    kLocalTime,
    kLocalTimeN,
    kLocalTimestamp,
    kLocalTimestampN,
    kCurrentRole,
    kCurrentUser,
    kSessionUser,
    kUser,
    kCurrentCatalog,
    kCurrentSchema,
};

class SQLValueFunction : public Expr {
public:
    SQLValueFunction() : Expr(pgcpp::nodes::NodeTag::kSQLValueFunction) {}

    Node* Clone() const override { return pgcpp::nodes::makePallocNode<SQLValueFunction>(*this); }
    bool Equals(const Node& other) const override {
        if (GetTag() != other.GetTag())
            return false;
        const auto& o = static_cast<const SQLValueFunction&>(other);
        return op == o.op && type == o.type && location == o.location;
    }

    SQLValueFunctionOp op = SQLValueFunctionOp::kCurrentDate;
    Oid type = 0;  // result type OID
    int location = -1;
};

// ---------------------------------------------------------------------------
// TargetEntry — one entry in the target list (SELECT output column).
// ---------------------------------------------------------------------------

class TargetEntry : public Expr {
public:
    TargetEntry() : Expr(pgcpp::nodes::NodeTag::kTargetEntry) {}

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
    RangeTblRef() : Node(pgcpp::nodes::NodeTag::kRangeTblRef) {}

    Node* Clone() const override;
    bool Equals(const Node& other) const override;

    int rtindex = 0;  // index into the range table
};

// ---------------------------------------------------------------------------
// JoinExpr — join expression in the join tree.
// ---------------------------------------------------------------------------

class JoinExpr : public Node {
public:
    JoinExpr() : Node(pgcpp::nodes::NodeTag::kJoinExpr) {}

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
    FromExpr() : Node(pgcpp::nodes::NodeTag::kFromExpr) {}

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
    return pgcpp::nodes::makePallocNode<T>();
}

// ---------------------------------------------------------------------------
// makeVar / makeConst / makeOpExpr / makeFuncExpr — convenience constructors.
// ---------------------------------------------------------------------------

Var* makeVar(int varno, int varattno, Oid vartype, int vartypmod, Oid varcollid, int varlevelsup,
             int location);

Const* makeConst(Oid consttype, int consttypmod, Oid constcollid, int constlen,
                 pgcpp::types::Datum constvalue, bool constisnull, bool constbyval, int location);

Const* makeNullConst(Oid consttype, int consttypmod, Oid constcollid);

}  // namespace pgcpp::parser

// parse_expr.cpp — Expression transformation for parse analysis.
//
// Converted from PostgreSQL 15's src/backend/parser/parse_expr.c.
// Transforms raw parse tree expressions (A_Expr, A_Const, ColumnRef, etc.)
// into transformed expression nodes (OpExpr, Const, Var, etc.).
#include "parser/parse_expr.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "parser/analyze.hpp"
#include "parser/parse_clause.hpp"
#include "parser/parse_coerce.hpp"
#include "parser/parse_func.hpp"
#include "parser/parse_oper.hpp"
#include "parser/parse_relation.hpp"
#include "parser/parse_type.hpp"
#include "types/datum.hpp"

namespace pgcpp::parser {

using pgcpp::catalog::kInvalidOid;
using pgcpp::catalog::Oid;
using pgcpp::nodes::Node;
using pgcpp::nodes::NodeTag;
using pgcpp::nodes::nodeTag;
using pgcpp::nodes::Value;
using pgcpp::types::kBoolOid;
using pgcpp::types::kFloat8Oid;
using pgcpp::types::kInt4Oid;
using pgcpp::types::kInt8Oid;
using pgcpp::types::kTextOid;
using pgcpp::types::kVarcharOid;

static constexpr Oid kUnknownOid = 705;

// VARHDRSZ — PostgreSQL's varlena header size, used when encoding typmods for
// varlena string types. Matches kVarHdrSz in src/types/builtins.cpp and
// src/parser/parse_type.cpp. Kept local to this translation unit to avoid
// pulling in additional headers; if it ever becomes shared, lift it to
// include/types/builtins.hpp.
static constexpr int32_t kVarHdrSz = 4;

// ExtractTypmodFromNode — local fallback for extracting an integer typmod
// from a TypeName::typmods entry list. The shared helper ComputeTypmod() in
// parse_type.cpp only handles entries that are bare Integer Value nodes, but
// gram.y's makeIntConst() emits integer literals as AConst nodes wrapping an
// Integer Value (see gram.yy:63-69). When LookupTypeName() returns typmod=-1
// for VARCHAR(N), this helper unwraps the AConst so coerce_type() receives
// the real typmod (VARHDRSZ + N).
//
// Mirrors the encoding rule in ComputeTypmod: varlena string types
// (varchar/text) encode the typmod as VARHDRSZ + max_chars; other types pass
// the integer through verbatim.
static int32_t ExtractTypmodFromNode(const std::vector<Node*>& typmods, Oid base_oid) {
    if (typmods.size() != 1)
        return -1;

    const Node* mod_node = typmods[0];
    if (mod_node == nullptr)
        return -1;

    // Bare Integer Value (the form ComputeTypmod already handles).
    if (mod_node->GetTag() == NodeTag::kInteger) {
        int64_t ival = static_cast<const Value*>(mod_node)->GetInteger();
        if (ival < 0)
            return -1;
        if (base_oid == kVarcharOid || base_oid == kTextOid)
            return static_cast<int32_t>(ival + kVarHdrSz);
        return static_cast<int32_t>(ival);
    }

    // AConst wrapping an Integer Value (the form gram.y's makeIntConst
    // emits for typmods like VARCHAR(5)).
    if (mod_node->GetTag() == NodeTag::kAConst) {
        const Node* inner = static_cast<const AConst*>(mod_node)->val;
        if (inner == nullptr || inner->GetTag() != NodeTag::kInteger)
            return -1;
        int64_t ival = static_cast<const Value*>(inner)->GetInteger();
        if (ival < 0)
            return -1;
        if (base_oid == kVarcharOid || base_oid == kTextOid)
            return static_cast<int32_t>(ival + kVarHdrSz);
        return static_cast<int32_t>(ival);
    }

    return -1;
}

// Forward declarations of internal transform functions.
static Node* transformColumnRef(ParseState* pstate, ColumnRef* cref);
static Node* transformAConst(ParseState* pstate, AConst* aconst);
static Node* transformAExpr(ParseState* pstate, AExpr* a);
static Node* transformFuncCall(ParseState* pstate, FuncCall* fn);
static Node* transformBoolExpr(ParseState* pstate, BoolExpr* b);
static Node* transformNullTest(ParseState* pstate, NullTest* n);
static Node* transformTypeCast(ParseState* pstate, TypeCast* tc);
static Node* transformCaseExpr(ParseState* pstate, CaseExpr* c);
static Node* transformSubLink(ParseState* pstate, SubLink* sublink);
static Node* transformParamRef(ParseState* pstate, ParamRef* pref);
static Node* transformCoalesceExpr(ParseState* pstate, CoalesceExpr* c);
static Node* transformMinMaxExpr(ParseState* pstate, MinMaxExpr* m);

// ---------------------------------------------------------------------------
// transformExpr — main entry point for expression transformation.
// ---------------------------------------------------------------------------

Node* transformExpr(ParseState* pstate, Node* expr, ParseExprKind exprKind) {
    if (expr == nullptr)
        return nullptr;

    ParseExprKind save_kind = pstate->p_expr_kind;
    pstate->p_expr_kind = exprKind;

    Node* result = transformExprRecurse(pstate, expr);

    pstate->p_expr_kind = save_kind;
    return result;
}

// ---------------------------------------------------------------------------
// transformExprRecurse — internal recursive transform dispatch.
// ---------------------------------------------------------------------------

Node* transformExprRecurse(ParseState* pstate, Node* expr) {
    if (expr == nullptr)
        return nullptr;

    NodeTag tag = nodeTag(expr);
    switch (tag) {
        case NodeTag::kColumnRef:
            return transformColumnRef(pstate, static_cast<ColumnRef*>(expr));
        case NodeTag::kAConst:
            return transformAConst(pstate, static_cast<AConst*>(expr));
        case NodeTag::kAExpr:
            return transformAExpr(pstate, static_cast<AExpr*>(expr));
        case NodeTag::kFuncCall:
            return transformFuncCall(pstate, static_cast<FuncCall*>(expr));
        case NodeTag::kBoolExpr:
            return transformBoolExpr(pstate, static_cast<BoolExpr*>(expr));
        case NodeTag::kNullTest:
            return transformNullTest(pstate, static_cast<NullTest*>(expr));
        case NodeTag::kTypeCast:
            return transformTypeCast(pstate, static_cast<TypeCast*>(expr));
        case NodeTag::kCaseExpr:
            return transformCaseExpr(pstate, static_cast<CaseExpr*>(expr));
        case NodeTag::kSubLink:
            return transformSubLink(pstate, static_cast<SubLink*>(expr));
        case NodeTag::kParamRef:
            return transformParamRef(pstate, static_cast<ParamRef*>(expr));
        case NodeTag::kCoalesceExpr:
            return transformCoalesceExpr(pstate, static_cast<CoalesceExpr*>(expr));
        case NodeTag::kMinMaxExpr:
            return transformMinMaxExpr(pstate, static_cast<MinMaxExpr*>(expr));

        // Already-transformed nodes: return as-is
        case NodeTag::kVar:
        case NodeTag::kConst:
        case NodeTag::kParam:
        case NodeTag::kOpExpr:
        case NodeTag::kFuncExpr:
        case NodeTag::kAggref:
        case NodeTag::kRelabelType:
        case NodeTag::kCoerceViaIO:
        case NodeTag::kScalarArrayOpExpr:
        case NodeTag::kCoerceToDomain:
            return expr;

        default:
            // For unhandled types, return as-is
            return expr;
    }
}

// ---------------------------------------------------------------------------
// transformColumnRef — resolve a column reference to a Var.
// ---------------------------------------------------------------------------

static Node* transformColumnRef(ParseState* pstate, ColumnRef* cref) {
    if (cref == nullptr || cref->fields.empty())
        return nullptr;

    int num_fields = static_cast<int>(cref->fields.size());

    // Case 1: unqualified column reference (e.g., "col")
    if (num_fields == 1) {
        Node* field0 = cref->fields[0];
        if (nodeTag(field0) == NodeTag::kString) {
            auto* v = static_cast<Value*>(field0);
            const std::string& colname = v->GetString();

            // Search the namespace for the column
            Node* var = scanNameSpaceForColumn(pstate, colname, cref->location);
            if (var != nullptr)
                return var;

            ereport(pgcpp::error::LogLevel::kError, "column does not exist");
            return nullptr;
        }
    }

    // Case 2: qualified column reference (e.g., "table.col")
    if (num_fields == 2) {
        Node* field0 = cref->fields[0];
        Node* field1 = cref->fields[1];

        if (nodeTag(field0) == NodeTag::kString && nodeTag(field1) == NodeTag::kString) {
            auto* tbl_val = static_cast<Value*>(field0);
            auto* col_val = static_cast<Value*>(field1);
            const std::string& tblname = tbl_val->GetString();
            const std::string& colname = col_val->GetString();

            // Find the RTE by table name
            int sublevels_up = 0;
            RangeTblEntry* rte = refnameRangeTblEntry(pstate, tblname.c_str(), &sublevels_up);
            if (rte == nullptr) {
                ereport(pgcpp::error::LogLevel::kError, "table does not exist");
                return nullptr;
            }

            Node* var = scanRTEForColumn(pstate, rte, colname, cref->location);
            if (var == nullptr) {
                ereport(pgcpp::error::LogLevel::kError, "column does not exist in table");
                return nullptr;
            }

            // Adjust varlevelsup for outer references
            if (sublevels_up > 0 && nodeTag(var) == NodeTag::kVar) {
                auto* v = static_cast<Var*>(var);
                v->varlevelsup = sublevels_up;
            }

            return var;
        }
    }

    // Case 3: table.* (star expansion)
    if (num_fields == 2) {
        Node* field1 = cref->fields[1];
        if (nodeTag(field1) == NodeTag::kAStar) {
            // This is a "table.*" reference — handled in target list expansion
            // Return the ColumnRef as-is for now; expandTargetList will handle it
            return cref;
        }
    }

    ereport(pgcpp::error::LogLevel::kError, "unrecognized column reference");
    return nullptr;
}

// ---------------------------------------------------------------------------
// transformAConst — transform an A_Const into a Const.
// ---------------------------------------------------------------------------

static Node* transformAConst(ParseState* pstate, AConst* aconst) {
    return make_const(pstate, aconst);
}

// ---------------------------------------------------------------------------
// transformAExpr — transform an A_Expr into an OpExpr, BoolExpr, etc.
// ---------------------------------------------------------------------------

static Node* transformAExpr(ParseState* pstate, AExpr* a) {
    Node* lexpr = a->lexpr ? transformExprRecurse(pstate, a->lexpr) : nullptr;
    Node* rexpr = a->rexpr ? transformExprRecurse(pstate, a->rexpr) : nullptr;

    // Get the operator name
    std::string opname;
    if (!a->name.empty()) {
        for (Node* n : a->name) {
            if (nodeTag(n) == NodeTag::kString) {
                auto* v = static_cast<Value*>(n);
                if (!opname.empty())
                    opname += " ";
                opname += v->GetString();
            }
        }
    }

    switch (a->kind) {
        case AExprKind::kOp: {
            // Handle boolean operators AND/OR/NOT as BoolExpr
            if (opname == "AND" && lexpr != nullptr && rexpr != nullptr) {
                std::vector<Node*> args = {lexpr, rexpr};
                auto* b = static_cast<BoolExpr*>(pgcpp::parser::make_andclause(std::move(args)));
                b->location = a->location;
                return b;
            }
            if (opname == "OR" && lexpr != nullptr && rexpr != nullptr) {
                std::vector<Node*> args = {lexpr, rexpr};
                auto* b = static_cast<BoolExpr*>(pgcpp::parser::make_orclause(std::move(args)));
                b->location = a->location;
                return b;
            }
            if (opname == "NOT" && rexpr != nullptr) {
                auto* b = static_cast<BoolExpr*>(pgcpp::parser::make_notclause(rexpr));
                b->location = a->location;
                return b;
            }

            // Binary operator: lexpr op rexpr
            if (lexpr != nullptr && rexpr != nullptr) {
                return make_op(pstate, opname, lexpr, rexpr, a->location);
            }
            break;
        }
        case AExprKind::kOpAny: {
            // expr op ANY (array)
            if (lexpr != nullptr && rexpr != nullptr) {
                return make_scalar_array_op(pstate, opname, true, lexpr, rexpr, a->location);
            }
            break;
        }
        case AExprKind::kOpAll: {
            // expr op ALL (array)
            if (lexpr != nullptr && rexpr != nullptr) {
                return make_scalar_array_op(pstate, opname, false, lexpr, rexpr, a->location);
            }
            break;
        }
        case AExprKind::kIn: {
            // expr IN (list/subquery) — opname is "=" for IN, "<>" for NOT IN
            if (lexpr != nullptr && rexpr != nullptr) {
                bool use_or = (opname == "=");
                return make_scalar_array_op(pstate, use_or ? "=" : "<>", use_or, lexpr, rexpr,
                                            a->location);
            }
            break;
        }
        case AExprKind::kNullif: {
            // NULLIF(a, b) — construct a NullIfExpr node. The "=" operator
            // is looked up via make_op (which also coerces the args to a
            // common type); the resulting OpExpr is then converted to a
            // NullIfExpr. opresulttype is the type of the first argument
            // (NULLIF returns arg1 or NULL of arg1's type).
            if (lexpr == nullptr || rexpr == nullptr) {
                break;
            }
            Node* op_expr = make_op(pstate, "=", lexpr, rexpr, a->location);
            if (op_expr == nullptr || op_expr->GetTag() != NodeTag::kOpExpr) {
                break;
            }
            auto* op = static_cast<OpExpr*>(op_expr);
            if (op->args.size() != 2) {
                break;
            }
            {
                auto* n = makeNode<NullIfExpr>();
                n->opno = op->opno;
                n->opresulttype = exprType(op->args[0]);
                n->opretset = false;
                n->opcollid = 0;
                n->inputcollid = 0;
                n->args = std::move(op->args);
                n->location = a->location;
                return n;
            }
        }
        case AExprKind::kDistinct: {
            // a IS DISTINCT FROM b
            if (lexpr != nullptr && rexpr != nullptr) {
                return make_op(pstate, "<>", lexpr, rexpr, a->location);
            }
            break;
        }
        case AExprKind::kNotDistinct: {
            // a IS NOT DISTINCT FROM b
            if (lexpr != nullptr && rexpr != nullptr) {
                return make_op(pstate, "=", lexpr, rexpr, a->location);
            }
            break;
        }
        case AExprKind::kLike: {
            // expr LIKE pattern — opname is "~~" for LIKE, "!~~" for NOT LIKE
            if (lexpr != nullptr && rexpr != nullptr) {
                return make_op(pstate, opname, lexpr, rexpr, a->location);
            }
            break;
        }
        case AExprKind::kIlike: {
            // ILIKE — opname is "~~*" for ILIKE, "!~~*" for NOT ILIKE
            if (lexpr != nullptr && rexpr != nullptr) {
                return make_op(pstate, opname, lexpr, rexpr, a->location);
            }
            break;
        }
        case AExprKind::kSimilar: {
            // SIMILAR TO — opname is "~" for SIMILAR TO, "!~" for NOT SIMILAR TO
            if (lexpr != nullptr && rexpr != nullptr) {
                return make_op(pstate, opname, lexpr, rexpr, a->location);
            }
            break;
        }
        case AExprKind::kBetween: {
            // expr BETWEEN low AND high => (expr >= low) AND (expr <= high)
            // rexpr is an AArrayExpr with [low, high] (returned as-is by
            // transformExprRecurse since kAArrayExpr has no dedicated transform).
            if (lexpr == nullptr || rexpr == nullptr)
                break;
            if (rexpr->GetTag() != NodeTag::kAArrayExpr)
                break;
            auto* arr = static_cast<AArrayExpr*>(rexpr);
            if (arr->elements.size() != 2)
                break;
            Node* low = transformExprRecurse(pstate, arr->elements[0]);
            Node* high = transformExprRecurse(pstate, arr->elements[1]);
            if (low == nullptr || high == nullptr)
                break;
            Node* ge = make_op(pstate, ">=", lexpr, low, a->location);
            Node* le = make_op(pstate, "<=", lexpr, high, a->location);
            std::vector<Node*> args = {ge, le};
            return make_andclause(std::move(args));
        }
        case AExprKind::kNotBetween: {
            // expr NOT BETWEEN low AND high => (expr < low) OR (expr > high)
            if (lexpr == nullptr || rexpr == nullptr)
                break;
            if (rexpr->GetTag() != NodeTag::kAArrayExpr)
                break;
            auto* arr = static_cast<AArrayExpr*>(rexpr);
            if (arr->elements.size() != 2)
                break;
            Node* low = transformExprRecurse(pstate, arr->elements[0]);
            Node* high = transformExprRecurse(pstate, arr->elements[1]);
            if (low == nullptr || high == nullptr)
                break;
            Node* lt = make_op(pstate, "<", lexpr, low, a->location);
            Node* gt = make_op(pstate, ">", lexpr, high, a->location);
            std::vector<Node*> args = {lt, gt};
            return make_orclause(std::move(args));
        }
        case AExprKind::kBetweenSym: {
            // expr BETWEEN SYMMETRIC low AND high =>
            //   ((lexpr >= low) AND (lexpr <= high))
            //   OR
            //   ((lexpr >= high) AND (lexpr <= low))
            // The OR-based expansion handles the case where low > high by
            // trying both orderings, avoiding the need for a CaseExpr.
            if (lexpr == nullptr || rexpr == nullptr)
                break;
            if (rexpr->GetTag() != NodeTag::kAArrayExpr)
                break;
            auto* arr = static_cast<AArrayExpr*>(rexpr);
            if (arr->elements.size() != 2)
                break;
            Node* low = transformExprRecurse(pstate, arr->elements[0]);
            Node* high = transformExprRecurse(pstate, arr->elements[1]);
            if (low == nullptr || high == nullptr)
                break;
            Node* ge1 = make_op(pstate, ">=", lexpr, low, a->location);
            Node* le1 = make_op(pstate, "<=", lexpr, high, a->location);
            Node* ge2 = make_op(pstate, ">=", lexpr, high, a->location);
            Node* le2 = make_op(pstate, "<=", lexpr, low, a->location);
            std::vector<Node*> and1 = {ge1, le1};
            std::vector<Node*> and2 = {ge2, le2};
            std::vector<Node*> or_args = {make_andclause(std::move(and1)),
                                          make_andclause(std::move(and2))};
            return make_orclause(std::move(or_args));
        }
        case AExprKind::kNotBetweenSym: {
            // expr NOT BETWEEN SYMMETRIC low AND high =>
            //   (lexpr < low OR lexpr > high)
            //   AND
            //   (lexpr < high OR lexpr > low)
            // Logically equivalent to NOT (BETWEEN SYMMETRIC); expanded as an
            // AND of two ORs to avoid constructing a NOT/CaseExpr wrapper.
            if (lexpr == nullptr || rexpr == nullptr)
                break;
            if (rexpr->GetTag() != NodeTag::kAArrayExpr)
                break;
            auto* arr = static_cast<AArrayExpr*>(rexpr);
            if (arr->elements.size() != 2)
                break;
            Node* low = transformExprRecurse(pstate, arr->elements[0]);
            Node* high = transformExprRecurse(pstate, arr->elements[1]);
            if (low == nullptr || high == nullptr)
                break;
            Node* lt1 = make_op(pstate, "<", lexpr, low, a->location);
            Node* gt1 = make_op(pstate, ">", lexpr, high, a->location);
            Node* lt2 = make_op(pstate, "<", lexpr, high, a->location);
            Node* gt2 = make_op(pstate, ">", lexpr, low, a->location);
            std::vector<Node*> or1 = {lt1, gt1};
            std::vector<Node*> or2 = {lt2, gt2};
            std::vector<Node*> and_args = {make_orclause(std::move(or1)),
                                           make_orclause(std::move(or2))};
            return make_andclause(std::move(and_args));
        }
        default:
            break;
    }

    ereport(pgcpp::error::LogLevel::kError, "unsupported expression type in transformAExpr");
    return nullptr;
}

// ---------------------------------------------------------------------------
// transformFuncCall — transform a FuncCall into a FuncExpr or Aggref.
// ---------------------------------------------------------------------------

static Node* transformFuncCall(ParseState* pstate, FuncCall* fn) {
    // Delegate to parse_func.cpp's transformFuncCall
    return pgcpp::parser::transformFuncCall(pstate, fn, fn->location);
}

// ---------------------------------------------------------------------------
// transformBoolExpr — transform AND/OR/NOT.
// ---------------------------------------------------------------------------

static Node* transformBoolExpr(ParseState* pstate, BoolExpr* b) {
    std::vector<Node*> targs;
    for (Node* arg : b->args) {
        targs.push_back(transformExprRecurse(pstate, arg));
    }

    auto* result = makeNode<BoolExpr>();
    result->boolop = b->boolop;
    result->args = targs;
    result->location = b->location;
    return result;
}

// ---------------------------------------------------------------------------
// transformNullTest — transform IS NULL / IS NOT NULL.
// ---------------------------------------------------------------------------

static Node* transformNullTest(ParseState* pstate, NullTest* n) {
    auto* result = makeNode<NullTest>();
    result->arg = n->arg ? transformExprRecurse(pstate, n->arg) : nullptr;
    result->nulltesttype = n->nulltesttype;
    result->argisrow = n->argisrow;
    result->location = n->location;
    return result;
}

// ---------------------------------------------------------------------------
// transformTypeCast — transform a type cast expression.
// ---------------------------------------------------------------------------

static Node* transformTypeCast(ParseState* pstate, TypeCast* tc) {
    Node* expr = tc->arg ? transformExprRecurse(pstate, tc->arg) : nullptr;
    if (expr == nullptr)
        return nullptr;

    // Get the target type from the TypeName.
    //
    // Use LookupTypeName() (rather than the string-only typenameTypeId()
    // overload) so that the typmod encoded in tn->typmods (e.g., the (5)
    // in VARCHAR(5)) is decoded into the integer typmod form PostgreSQL
    // uses internally (VARHDRSZ + max_chars for varlena string types).
    // The tn->typemod field is unused — it stays at its default 0 — so
    // reading it directly would silently drop the typmod and bypass
    // varchar_in's truncation logic.
    Oid target_type = kUnknownOid;
    int target_typmod = -1;
    if (tc->type_name != nullptr) {
        int32_t computed_typmod = -1;
        target_type = LookupTypeName(pstate, tc->type_name, &computed_typmod);
        if (target_type != kInvalidOid) {
            target_typmod = computed_typmod;
        }
        // LookupTypeName() -> ComputeTypmod() only handles typmods entries
        // that are bare Integer Value nodes. Our gram.y emits integer
        // literals via makeIntConst() which wraps the Value in an AConst,
        // so ComputeTypmod() returns -1 for VARCHAR(N). Extract the typmod
        // locally in that case so coerce_type() receives a real typmod.
        if (target_typmod < 0 && !tc->type_name->typmods.empty() && target_type != kInvalidOid) {
            int32_t local_typmod = ExtractTypmodFromNode(tc->type_name->typmods, target_type);
            if (local_typmod >= 0)
                target_typmod = local_typmod;
        }
    }

    if (target_type == kUnknownOid || target_type == kInvalidOid) {
        return expr;  // can't cast, return as-is
    }

    Oid expr_type = exprType(expr);
    return coerce_type(pstate, expr, expr_type, target_type, target_typmod,
                       CoercionContext::kExplicit, CoercionForm::kExplicit, tc->location);
}

// ---------------------------------------------------------------------------
// transformCaseExpr — transform a CASE expression.
// ---------------------------------------------------------------------------

static Node* transformCaseExpr(ParseState* pstate, CaseExpr* c) {
    auto* result = makeNode<CaseExpr>();
    result->casetype = kUnknownOid;  // will be resolved from WHEN/ELSE results
    result->casecollid = 0;
    result->arg = c->arg ? transformExprRecurse(pstate, c->arg) : nullptr;
    result->location = c->location;

    Oid common_type = kUnknownOid;

    for (Node* when_node : c->args) {
        if (nodeTag(when_node) == NodeTag::kCaseWhen) {
            auto* when = static_cast<CaseWhen*>(when_node);
            auto* new_when = makeNode<CaseWhen>();
            new_when->expr = when->expr ? transformExprRecurse(pstate, when->expr) : nullptr;
            new_when->result = when->result ? transformExprRecurse(pstate, when->result) : nullptr;
            new_when->location = when->location;
            result->args.push_back(new_when);

            // Track common type
            if (new_when->result != nullptr) {
                Oid result_type = exprType(new_when->result);
                if (common_type == kUnknownOid) {
                    common_type = result_type;
                } else {
                    std::vector<Node*> tmp = {result->defresult, new_when->result};
                    common_type = select_common_type(pstate, tmp, "CASE", nullptr);
                }
            }
        }
    }

    result->defresult = c->defresult ? transformExprRecurse(pstate, c->defresult) : nullptr;

    // Set the case type
    if (result->defresult != nullptr) {
        Oid def_type = exprType(result->defresult);
        if (common_type == kUnknownOid) {
            common_type = def_type;
        }
    }
    result->casetype = common_type;

    return result;
}

// ---------------------------------------------------------------------------
// transformSubLink — transform a subquery expression.
// ---------------------------------------------------------------------------

static Node* transformSubLink(ParseState* pstate, SubLink* sublink) {
    auto* result = makeNode<SubLink>();
    result->sublinktype = sublink->sublinktype;
    result->sublinkid = sublink->sublinkid;
    result->testexpr =
        sublink->testexpr ? transformExprRecurse(pstate, sublink->testexpr) : nullptr;
    result->opername = sublink->opername;
    result->location = sublink->location;

    // Transform the subselect — this requires analyzing the subquery
    // For now, we keep the raw parse tree as the subselect
    // The full implementation would call parse_analyze on the subselect
    if (sublink->subselect != nullptr) {
        // Check if it's already a Query (already analyzed) or a raw SelectStmt
        if (nodeTag(sublink->subselect) == NodeTag::kQuery) {
            result->subselect = sublink->subselect;
        } else {
            // Transform the subquery using a new ParseState
            ParseState* sub_pstate = make_parsestate(pstate);
            sub_pstate->p_expr_kind = ParseExprKind::kFromSubselect;
            Query* subquery = transformStmt(sub_pstate, sublink->subselect);
            free_parsestate(sub_pstate);
            result->subselect = subquery;
        }
        pstate->p_has_sub_links = true;
    }

    return result;
}

// ---------------------------------------------------------------------------
// transformParamRef — transform a parameter reference ($1, $2, etc.).
// ---------------------------------------------------------------------------

static Node* transformParamRef([[maybe_unused]] ParseState* pstate, ParamRef* pref) {
    auto* param = makeNode<Param>();
    param->paramkind = ParamKind::kExtern;
    param->paramid = pref->number;
    param->paramtype = kUnknownOid;  // type will be resolved later
    param->paramtypmod = -1;
    param->paramcollid = 0;
    param->location = pref->location;
    return param;
}

// ---------------------------------------------------------------------------
// transformCoalesceExpr — transform a COALESCE expression.
//
// Mirrors PostgreSQL's transformCoalesceExpr in parse_expr.c: transform each
// argument, select a common type, and use it as coalescetype. pgcpp (like its
// transformCaseExpr) does not insert explicit coercion nodes between args of
// differing types; arguments retain their transformed shape and the executor
// simply returns the first non-NULL value.
// ---------------------------------------------------------------------------

static Node* transformCoalesceExpr(ParseState* pstate, CoalesceExpr* c) {
    auto* result = makeNode<CoalesceExpr>();
    result->location = c->location;

    for (Node* arg : c->args) {
        result->args.push_back(transformExprRecurse(pstate, arg));
    }

    if (result->args.empty()) {
        ereport(pgcpp::error::LogLevel::kError, "COALESCE requires at least one argument");
        return nullptr;
    }

    result->coalescetype = select_common_type(pstate, result->args, "COALESCE", nullptr);
    result->coalescecollid = 0;
    return result;
}

// ---------------------------------------------------------------------------
// transformMinMaxExpr — transform a GREATEST/LEAST expression.
//
// Mirrors PostgreSQL's transformMinMaxExpr in parse_expr.c: transform each
// argument and resolve the common type for inputcollid. pgcpp does not insert
// explicit coercion nodes between args of differing types (matches the
// transformCaseExpr convention); the executor's MinMaxExpr branch compares
// argument Datums directly.
// ---------------------------------------------------------------------------

static Node* transformMinMaxExpr(ParseState* pstate, MinMaxExpr* m) {
    auto* result = makeNode<MinMaxExpr>();
    result->minmaxtype = m->minmaxtype;
    result->location = m->location;

    for (Node* arg : m->args) {
        result->args.push_back(transformExprRecurse(pstate, arg));
    }

    if (result->args.empty()) {
        ereport(pgcpp::error::LogLevel::kError, "GREATEST/LEAST requires at least one argument");
        return nullptr;
    }

    result->minmaxcollid = 0;
    result->inputcollid = select_common_type(pstate, result->args, "GREATEST/LEAST", nullptr);
    return result;
}

}  // namespace pgcpp::parser
